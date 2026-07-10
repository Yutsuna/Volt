require "./NativeTable"


module Volt::Compiler


  class FunctionEmiter

    #------------------------------------------------------------------------------------

    def initialize( name : String, arity : Int32,
                    @func_index : Hash( String, Int32 ),
                    @signatures : Hash( String, Frontend::FuncSig ),
                    @natives : NativeTable,
                    @types : Hash( String, Frontend::TypeInfo ) = {} of String => Frontend::TypeInfo,
                    # `self`'s owning type when compiling a method chunk, or nil for a free
                    # function : drives InstanceVar / implicit-self lowering.
                    @self_owner : Frontend::TypeInfo? = nil,
                    # `@@var` -> global slot, shared across the whole unit.
                    @global_index : Hash( String, Int32 ) = {} of String => Int32,
                    # Unmangled name of the method being compiled (nil for a free
                    # function / main) : what a `super` inside the body resolves to
                    # on the superclass chain.
                    @method_name : String? = nil )
      @chunk    = IR::Chunk.new( name, arity )
      @scope    = {} of String => Int32
      @next_reg = 0
      @max_reg  = 0
      @scopes_resource_registers = [ [] of Tuple( Int32, Int32 ) ]
      @loop_stack = [] of LoopCtx
    end

    # Per-`while` bookkeeping for `break`/`next`, pushed/popped around
    # `compile_body` in `compile_while` (nesting = a stack). `continue_target`
    # is the pc of the condition re-check (`next` jumps straight there,
    # skipping the rest of the current iteration's body — no scope to unwind:
    # `enter_scope`/`exit_scope` only ever wrap a whole function body, never a
    # loop body, so no `DROP_SCOPE` is ever skipped by jumping out early).
    # `result_reg` is the loop's overall value (nil unless a `break value`
    # writes into it — Ruby/Crystal semantics); `break_jumps` collects the
    # placeholder `JMP`s emitted for each `break`, patched to land just past
    # the loop once its final address is known.
    private struct LoopCtx
      getter continue_target : Int32
      getter result_reg      : Int32
      getter break_jumps     : Array( Int32 )

      def initialize( @continue_target : Int32, @result_reg : Int32 )
        @break_jumps = [] of Int32
      end
    end

    #------------------------------------------------------------------------------------

    def bind_param( name : String, type : Frontend::Type ) : Int32
      base = alloc_block( slot_count( type ) )
      @scope[ name ] = base
      base
    end

    # The register `self` is bound to — used by `BytecodeCompiler` to return
    # `self` directly from a struct's `initialize` (see `compile_struct_new`).
    def self_register : Int32
      @scope[ "self" ]
    end

    # Number of contiguous register slots `type` occupies (architecture #1.B):
    # 1 for any scalar or class reference, N for a struct value (N = that
    # struct's own slot count, flattening nested structs in turn).
    def slot_count( type : Frontend::Type ) : Int32
      if type.is_a?( Frontend::NominalType ) && type.kind.struct?
        type.reg_layout.try( &.total_size ) || 1
      else
        1
      end
    end

    def finish : IR::Chunk
      @chunk.num_registers = @max_reg
      @chunk
    end

    #------------------------------------------------------------------------------------

    def emit_ret( reg : Int32, slots : Int32 = 1 )
      emit_abx( IR::Opcode::RET, reg, slots )
    end

    def enter_scope
      @scopes_resource_registers.push( [] of Tuple( Int32, Int32 ) )
    end

    def exit_scope
      current_pc = here.to_u32
      scope = @scopes_resource_registers.pop
      regs  = scope.map { |reg, _| reg }

      unless regs.empty?
        table_idx = add_scope_table( regs )
        regs.each do |r|
          emit_abc( IR::Opcode::DROP, r, 0, 0 )
        end

        @chunk.drop_map.entries.each do |entry|
          if entry.pc_end == UInt32::MAX && regs.includes?( entry.register.to_i32 )
            entry.pc_end = current_pc
          end
        end
      end
    end

    def track_raii_resource( reg : Int32, type_id : Int32 )
      @scopes_resource_registers.last << { reg, type_id }
      @chunk.drop_map.entries << IR::DropEntry.new( here.to_u32, UInt32::MAX, reg.to_u8, type_id )
    end

    # Style-1 constructor shorthand (`def initialize( @x : T )`) has no
    # corresponding `@x = x` statement in the body : the field store is
    # implicit, so the compiler emits it directly from the already-bound
    # parameter register.
    def store_ivar_param( owner : Frontend::TypeInfo, name : String ) : Nil
      param_reg = @scope[ name ]
      self_reg  = @scope[ "self" ]
      slot      = field_slot_index( owner.name, name )
      n         = find_field( owner.name, name ).try { |f| slot_count( f.type ) } || 1
      if owner.kind.struct?
        place_value( self_reg + slot, param_reg, n )
      else
        n.times { |i| emit_abc( IR::Opcode::STORE_FIELD, self_reg, slot + i, param_reg + i ) }
      end
    end

    # `__drop_fields` codegen: DROP one reference field off `self`.
    def emit_drop_field( owner : Frontend::TypeInfo, field : Frontend::FieldSlot ) : Nil
      self_reg = @scope[ "self" ]
      slot     = field_slot_index( owner.name, field.name )
      tmp      = alloc
      emit_abc( IR::Opcode::LOAD_FIELD, tmp, self_reg, slot )
      type_id  = field.type.as?( Frontend::NominalType ).try( &.type_id ) || -1
      emit_abx( IR::Opcode::DROP, tmp, type_id )
    end

    def emit_ret_nil : Nil
      r = alloc
      emit_abc( IR::Opcode::LOAD_NIL, r, 0, 0 )
      emit_ret( r, 1 )
    end


    #------------------------------------------------------------------------------------

    # `drop_tail_ctor`: whether a bare `Type.new(...)` in TAIL position also
    # gets tracked for `exit_scope` to DROP. A function/method body passes
    # `false` when its signature carries an explicit `-> SomeClass`
    # annotation — that's the API contract declaring the constructed value
    # escapes to the caller, so `exit_scope` (which runs before `RET`) must
    # not free it out from under the return. Every other body — a function
    # with no return annotation (its bare tail ctor is fire-and-forget, e.g.
    # `def f; Logger.new(...); end` — nothing ever reads the "return"), and
    # every nested `if`/`while` block — keeps the default `true`.
    def compile_body( nodes : Array( Frontend::ANode ), drop_tail_ctor : Bool = true ) : Int32
      last = -1
      nodes.each_with_index do |node, i|
        expr = node.as( Frontend::AExpr )
        last = compile_expr( expr )
        is_tail = i == nodes.size - 1
        if ( !is_tail || drop_tail_ctor ) && ctor_temp?( expr ) &&
           ( t = expr.resolved_type ).is_a?( Frontend::NominalType ) && t.kind.object?
          track_raii_resource( last, t.type_id )
        end
      end
      if last < 0
        last = alloc
        emit_abc( IR::Opcode::LOAD_NIL, last, 0, 0 )
      end
      last
    end

    # True for a bare constructor call (`Type.new` / `Type.new(...)`) — the
    # only statement-position expression that *creates* an unowned object.
    private def ctor_temp?( expr : Frontend::AExpr ) : Bool
      return false unless expr.is_a?( Frontend::MethodCall ) || expr.is_a?( Frontend::MemberAccess )
      return false unless expr.name == "new"
      recv = expr.receiver
      recv.is_a?( Frontend::Ident ) && !@scope.has_key?( recv.name ) && @types.has_key?( recv.name )
    end

    def compile_expr( expr : Frontend::AExpr ) : Int32
      case expr
      when Frontend::IntLit     then const_reg( IR::Value.int( expr.value ) )
      when Frontend::FloatLit   then const_reg( IR::Value.float( expr.value ) )
      when Frontend::StringLit  then compile_string_lit( expr.value )
      when Frontend::RegexLit
        const_reg( IR::Value.regex( ::Regex.new( expr.value ) ) )
      when Frontend::BoolLit
        r = alloc
        emit_abc( expr.value ? IR::Opcode::LOAD_TRUE : IR::Opcode::LOAD_FALSE, r, 0, 0 )
        r
      when Frontend::NilLit
        r = alloc
        emit_abc( IR::Opcode::LOAD_NIL, r, 0, 0 )
        r
      when Frontend::Ident        then compile_ident( expr )
      when Frontend::InstanceVar  then compile_instance_var( expr )
      when Frontend::ClassVar     then compile_class_var( expr )
      when Frontend::MemberAccess then compile_member_access( expr )
      when Frontend::SelfExpr     then @scope[ "self" ]
      when Frontend::SuperCall    then compile_super_call( expr )
      when Frontend::Assign       then compile_assign( expr )
      when Frontend::BinaryOp     then compile_binary( expr )
      when Frontend::UnaryOp      then compile_unary( expr )
      when Frontend::Call         then compile_call( expr )
      when Frontend::MethodCall   then compile_method_call( expr )
      when Frontend::PipeExpr     then compile_expr( expr.desugared.not_nil! )
      when Frontend::IfExpr     then compile_if( expr )
      when Frontend::WhileExpr  then compile_while( expr )
      when Frontend::ReturnExpr then compile_return( expr )
      when Frontend::BreakExpr  then compile_break( expr )
      when Frontend::NextExpr   then compile_next( expr )
      when Frontend::RaiseExpr
        val_reg = compile_expr( expr.value )
        emit_abc( IR::Opcode::RAISE, val_reg, 0, 0 )
        val_reg
      when Frontend::TypeofExpr
        t = expr.resolved_operand_type || Frontend::Type::UNKNOWN
        # An adopted mixin copy stamps `typeof self` with the mixin's own
        # name (single typecheck) : re-resolve against the real includer.
        if expr.operand.is_a?( Frontend::SelfExpr ) && ( owner = @self_owner )
          t = nominal_of( owner )
        end
        compile_string_lit( t.to_s )
      else
        raise "internal: unlowerable node #{expr.class.name} (should be rejected by Semantic)"
      end
    end

    #------------------------------------------------------------------------------------

    private def alloc : Int32
      r          = @next_reg
      @next_reg += 1
      @max_reg   = @next_reg if @next_reg > @max_reg
      r
    end

    private def alloc_block( n : Int32 ) : Int32
      base       = @next_reg
      @next_reg += n
      @max_reg   = @next_reg if @next_reg > @max_reg
      base
    end

    private def emit_abc( op : IR::Opcode, a : Int32, b : Int32, c : Int32 )
      @chunk.code << IR::Instruction.abc( op, a, b, c )
    end

    private def emit_abx( op : IR::Opcode, a : Int32, bx : Int32 )
      @chunk.code << IR::Instruction.abx( op, a, bx )
    end

    private def here : Int32
      @chunk.code.size
    end

    private def emit_jump_placeholder( op : IR::Opcode, a : Int32 ) : Int32
      idx = here
      emit_abx( op, a, 0 )
      idx
    end

    private def patch_jump( at : Int32, target : Int32 )
      old = @chunk.code[ at ]
      @chunk.code[ at ] = IR::Instruction.abx( old.op, old.a, target )
    end

    private def add_const( v : IR::Value ) : Int32
      @chunk.constants << v
      @chunk.constants.size - 1
    end

    private def add_scope_table( regs : Array( Int32 ) ) : Int32
      @chunk.scope_tables << regs
      @chunk.scope_tables.size - 1
    end

    private def emit_scope_cleanup
      @scopes_resource_registers.reverse_each do |scope|
        scope.each do |reg, type_id|
          emit_abx( IR::Opcode::DROP, reg, type_id )
        end
      end
    end

    private def const_reg( v : IR::Value ) : Int32
      r   = alloc
      idx = add_const( v )
      emit_abx( IR::Opcode::LOAD_CONST, r, idx )
      r
    end

    private def compile_assign( expr : Frontend::Assign ) : Int32
      case target = expr.target
      when Frontend::Ident        then compile_assign_ident( expr, target )
      when Frontend::InstanceVar  then compile_assign_ivar( expr, target )
      when Frontend::ClassVar     then compile_assign_class_var( expr, target )
      when Frontend::MemberAccess then compile_assign_member( expr, target )
      when Frontend::UnaryOp
        if target.op.star?
          compile_assign_deref( expr, target )
        else
          raise "internal: unlowerable assignment target #{expr.target.class}"
        end
      else
        raise "internal: unlowerable assignment target #{expr.target.class}"
      end
    end

    private def compile_assign_deref( expr : Frontend::Assign, target : Frontend::UnaryOp ) : Int32
      ptr_reg = compile_expr( target.operand )
      val_reg = compile_expr( expr.value )
      width = IR::PtrWidth.for( target.resolved_type.not_nil! )
      emit_abc( IR::Opcode::STORE_PTR, ptr_reg, val_reg, width.value )
      val_reg
    end

    # `@@name` lives in a process-global slot (modules have no instances).
    private def compile_class_var( expr : Frontend::ClassVar ) : Int32
      dest = alloc
      emit_abx( IR::Opcode::LOAD_GLOBAL, dest, global_slot( expr.name ) )
      dest
    end

    private def compile_assign_class_var( expr : Frontend::Assign, target : Frontend::ClassVar ) : Int32
      value_reg = compile_expr( expr.value )
      emit_abx( IR::Opcode::STORE_GLOBAL, value_reg, global_slot( target.name ) )
      value_reg
    end

    private def global_slot( name : String ) : Int32
      owner = @self_owner.not_nil!
      @global_index[ "#{owner.name}::#{name}" ]
    end

    private def compile_assign_ident( expr : Frontend::Assign, target : Frontend::Ident ) : Int32
      name = target.name
      if @chunk.name == "main" && !@scope.has_key?( name ) && @global_index.has_key?( name )
        value_reg = compile_expr( expr.value )
        n = slot_count( target.resolved_type || Frontend::Type::UNKNOWN )
        base_slot = @global_index[ name ]
        n.times do |i|
          emit_abx( IR::Opcode::STORE_GLOBAL, value_reg + i, base_slot + i )
        end
        return value_reg
      end

      value_reg = compile_expr( expr.value )
      if home = @scope[ name ]?
        place_value( home, value_reg, slot_count( target.resolved_type || Frontend::Type::UNKNOWN ) )
        home
      else
        # First binding. A bare `x = y` / `x = self` RHS compiles to the
        # *existing* name's home register — binding `x` to it directly would
        # alias the two names (rebinding either then clobbers the other), and
        # `track_raii_resource` below would put that shared register into
        # `raii_regs`, which `execute` nil-inits at frame entry — for a
        # parameter that wipes the argument before the body ever reads it.
        # Copy into a fresh home register instead.
        if expr.value.is_a?( Frontend::Ident ) || expr.value.is_a?( Frontend::SelfExpr )
          n    = slot_count( target.resolved_type || Frontend::Type::UNKNOWN )
          home = n <= 1 ? alloc : alloc_block( n )
          place_value( home, value_reg, n )
          value_reg = home
        end
        @scope[ name ] = value_reg
        if ( t = expr.value.resolved_type ).is_a?( Frontend::NominalType ) && t.kind.object?
          track_raii_resource( value_reg, t.type_id )
        end
        value_reg
      end
    end

    # `@x = value` : only meaningful inside an instance method (`@self_owner`
    # set). A struct `self` is a register range: the field *is* a register,
    # so writing it is a plain move/copy, no opcode. A class `self` is a heap
    # reference: `STORE_FIELD` per slot (a multi-slot struct field flattens
    # into N consecutive slots in the object's `fields` array).
    private def compile_assign_ivar( expr : Frontend::Assign, target : Frontend::InstanceVar ) : Int32
      owner     = @self_owner.not_nil!
      self_reg  = @scope[ "self" ]
      slot      = field_slot_index( owner.name, target.name )
      field     = find_field( owner.name, target.name )
      value_reg = compile_expr( expr.value )
      n         = slot_count( field.try( &.type ) || target.resolved_type || Frontend::Type::UNKNOWN )

      if owner.kind.struct?
        place_value( self_reg + slot, value_reg, n )
        self_reg + slot
      else
        n.times { |i| emit_abc( IR::Opcode::STORE_FIELD, self_reg, slot + i, value_reg + i ) }
        value_reg
      end
    end

    # `obj.field = value` : Phase 2 only supports this on class receivers
    # (`obj` a heap reference); assigning through a struct-valued receiver
    # expression isn't addressable the same way and isn't exercised yet.
    private def compile_assign_member( expr : Frontend::Assign, target : Frontend::MemberAccess ) : Int32
      recv_ty = target.receiver.resolved_type
      raise "internal: member assignment on non-object receiver" unless recv_ty.is_a?( Frontend::NominalType )
      recv_reg  = compile_expr( target.receiver )
      slot      = field_slot_index( recv_ty.name, target.name )
      value_reg = compile_expr( expr.value )
      n         = slot_count( target.resolved_type || Frontend::Type::UNKNOWN )
      n.times { |i| emit_abc( IR::Opcode::STORE_FIELD, recv_reg, slot + i, value_reg + i ) }
      value_reg
    end

    private def compile_ident( expr : Frontend::Ident ) : Int32
      if reg = @scope[ expr.name ]?
        return reg
      end

      if @global_index.has_key?( expr.name )
        n = slot_count( expr.resolved_type || Frontend::Type::UNKNOWN )
        dest = alloc_block( n )
        base_slot = @global_index[ expr.name ]
        n.times do |i|
          emit_abx( IR::Opcode::LOAD_GLOBAL, dest + i, base_slot + i )
        end
        return dest
      end

      # Parenthesis-less zero-arg call on `self` (mirrors `compile_call`'s
      # self-dispatch and `TypeChecker#infer_ident`) : `boot_sequence`.
      if ( owner = @self_owner ) && !@scope.has_key?( expr.name ) && find_method( owner.name, expr.name )
        self_reg = @scope[ "self" ]
        if owner.kind.struct?
          return compile_struct_call( nominal_of( owner ), expr.name, self_reg, [] of Int32, [] of Frontend::Type )
        else
          return compile_virtual_call( nominal_of( owner ), expr.name, self_reg, [] of Int32, [] of Frontend::Type )
        end
      end

      if sig = @signatures[ expr.name ]?
        base = alloc_block( Math.max( 1, slot_count( sig.ret ) ) )
        if sig.extern
          emit_abc( IR::Opcode::CALL_NATIVE, base, @natives.intern( sig.lib, expr.name ), 0 )
        else
          emit_abc( IR::Opcode::CALL, base, @func_index[ expr.name ], 0 )
        end
        return base
      end

      raise "internal: unbound identifier #{expr.name}"
    end

    private def compile_instance_var( expr : Frontend::InstanceVar ) : Int32
      owner    = @self_owner.not_nil!
      self_reg = @scope[ "self" ]
      slot     = field_slot_index( owner.name, expr.name )
      # A mixin body type-checks once with `self` bound to the mixin itself
      # (no fields), so `@price` resolves to `UNKNOWN` there and that gets
      # cached on the shared AST node. Once adopted per-includer (see
      # `BytecodeCompiler#collect_method_jobs`), `owner` here is the real
      # including class, so refresh the node's type from its actual field —
      # `compile_binary`'s float/int opcode choice reads this right after.
      field = find_field( owner.name, expr.name )
      expr.resolved_type = field.type if field
      if owner.kind.struct?
        self_reg + slot
      else
        load_field_block( self_reg, slot, slot_count( field.try( &.type ) || Frontend::Type::UNKNOWN ) )
      end
    end

    # Reads `n` consecutive slots of a heap object's field region (`n > 1` for
    # a nested struct-valued field) into a fresh contiguous register block.
    private def load_field_block( obj_reg : Int32, slot : Int32, n : Int32 ) : Int32
      dest = alloc_block( n )
      n.times { |i| emit_abc( IR::Opcode::LOAD_FIELD, dest + i, obj_reg, slot + i ) }
      dest
    end

    # A string literal lowers to `String.new( <bytes>, <size>, false )` on the
    # Core `String` class : a `Tag::Ptr` constant into the chunk-rooted,
    # null-terminated bytes plus the compile-time byte size — a *borrowed*
    # instance (`owned = false`), so its `finalize` never frees static memory.
    # Without the Core (bare unit specs), raises a compiler error.
    private def compile_string_lit( value : String ) : Int32
      info = @types[ "String" ]?
      unless info && info.kind.class? && info.initializer
        raise "String class must be defined to use string literals"
      end

      @chunk.strings << value
      ptr_reg   = const_reg( IR::Value.ptr( value.to_unsafe.as( Void* ) ) )
      size_reg  = const_reg( IR::Value.int( value.bytesize.to_i64 ) )
      owned_reg = alloc
      emit_abc( IR::Opcode::LOAD_FALSE, owned_reg, 0, 0 )

      obj = alloc
      emit_abx( IR::Opcode::INIT_OBJ, obj, info.type_id )
      window = alloc_block( 5 )
      place_value( window + 1, obj, 1 )
      place_value( window + 2, ptr_reg, 1 )
      place_value( window + 3, size_reg, 1 )
      place_value( window + 4, owned_reg, 1 )
      emit_abc( IR::Opcode::CALL, window, @func_index[ init_chunk_name( info, 3 ) ], 4 )
      obj
    end

    private def compile_member_access( expr : Frontend::MemberAccess ) : Int32
      return compile_to_string( expr.receiver ) if expr.name == "to_string"

      # `Book.new` and `GeoCoordinate.new` : a parenthesis-less constructor call on a nominal type
      if ( recv = expr.receiver ).is_a?( Frontend::Ident ) && !@scope.has_key?( recv.name ) &&
         ( info = @types[ recv.name ]? )
        recv.resolved_type = nominal_of( info )
        if expr.name == "new"
          return compile_constructor_call( info, [] of Frontend::AExpr )
        end
      end

      # `Module.method` : a parenthesis-less zero-arg call on a module type
      if ( recv = expr.receiver ).is_a?( Frontend::Ident ) && !@scope.has_key?( recv.name ) &&
         ( info = @types[ recv.name ]? ) && info.kind.module?
        recv.resolved_type = nominal_of( info )
        return compile_static_call( info.name, expr.name, [] of Int32, [] of Frontend::Type )
      end

      recv_ty = expr.receiver.resolved_type
      unless recv_ty.is_a?( Frontend::NominalType )
        # Parenthesis-less zero-arg method call on a primitive receiver.
        if info = primitive_owner( recv_ty, expr.name )
          r = compile_expr( expr.receiver )
          return compile_primitive_call( info, expr.name, r, [] of Int32, [] of Frontend::Type )
        end
        raise "internal: member access on non-object type"
      end
      recv_reg = compile_expr( expr.receiver )

      unless find_field( recv_ty.name, expr.name )
        # A bare zero-arg method call (`book.description`, reclassified from
        # field access by the parser/Semantic) : a struct resolves straight
        # to a direct call (no polymorphism possible) ; a class goes through
        # the vtable, same as the parenthesised case in `compile_method_call`.
        if find_method( recv_ty.name, expr.name )
          if recv_ty.kind.struct?
            return compile_struct_call( recv_ty, expr.name, recv_reg, [] of Int32, [] of Frontend::Type )
          else
            return compile_virtual_call( recv_ty, expr.name, recv_reg, [] of Int32, [] of Frontend::Type )
          end
        end
        raise "internal: zero-arg method calls are not yet lowered (Phase 4) : #{recv_ty.name}##{expr.name}"
      end

      slot  = field_slot_index( recv_ty.name, expr.name )
      field = find_field( recv_ty.name, expr.name )
      if recv_ty.kind.struct?
        recv_reg + slot
      else
        load_field_block( recv_reg, slot, slot_count( field.try( &.type ) || Frontend::Type::UNKNOWN ) )
      end
    end

    private def compile_to_string( receiver : Frontend::AExpr ) : Int32
      # A user type that defines its own `to_string` wins over the builtin
      # stringification : `@position.to_string` must run `GeoCoordinate#to_string`,
      # not print `<object:..>`. Structs resolve directly, classes go virtual.
      recv_ty = receiver.resolved_type
      # A mixin body type-checks once with `self` bound to the mixin itself,
      # so a bare self-call (`to_string` in `Inspectable#inspect`) is stamped
      # `UNKNOWN` on the shared AST node. In the adopted per-includer copy
      # `@self_owner` is the real class : re-resolve the ident against it
      # (same refresh as `compile_instance_var`).
      if ( recv_ty.nil? || recv_ty.kind.unknown? ) && receiver.is_a?( Frontend::Ident ) &&
         ( owner = @self_owner ) && !@scope.has_key?( receiver.name ) &&
         ( m = find_method( owner.name, receiver.name ) )
        recv_ty = m.ret
        receiver.resolved_type = recv_ty
      end
      if recv_ty.is_a?( Frontend::NominalType ) && find_method( recv_ty.name, "to_string" )
        r = compile_expr( receiver )
        if recv_ty.kind.struct?
          return compile_struct_call( recv_ty, "to_string", r, [] of Int32, [] of Frontend::Type )
        else
          return compile_virtual_call( recv_ty, "to_string", r, [] of Int32, [] of Frontend::Type )
        end
      end

      # A reopened primitive's `to_string` wins over the builtin fallback.
      if info = primitive_owner( recv_ty, "to_string" )
        r = compile_expr( receiver )
        return compile_primitive_call( info, "to_string", r, [] of Int32, [] of Frontend::Type )
      end

      raise "Compile error: type '#{recv_ty}' does not implement to_string"
    end

    private def compile_method_call( expr : Frontend::MethodCall ) : Int32
      if expr.name == "includes?" && expr.receiver.is_a?( Frontend::RangeExpr )
        return compile_range_includes( expr )
      end
      return compile_to_string( expr.receiver ) if expr.name == "to_string"

      if ( recv = expr.receiver ).is_a?( Frontend::Ident ) && !@scope.has_key?( recv.name ) &&
         ( info = @types[ recv.name ]? )
        if expr.name == "new"
          return compile_constructor_call( info, expr.args )
        end
        if info.kind.module?
          arg_regs  = expr.args.map { |a| compile_expr( a ) }
          arg_types = expr.args.map { |a| a.resolved_type || Frontend::Type::UNKNOWN }
          return compile_static_call( info.name, expr.name, arg_regs, arg_types )
        end
      end

      # Structs have no subclassing/mixins to dispatch through — a method
      # call on a struct value always resolves to exactly one chunk, so it
      # compiles straight to a direct `CALL`. A class receiver goes through
      # the vtable instead (`compile_virtual_call`), since the receiver's
      # runtime type may be a subclass of its static type.
      recv_ty = expr.receiver.resolved_type
      # An explicit `obj.finalize()` never dispatches virtually (no vtable
      # slot exists for it) — resolved statically to the receiver's own or
      # nearest ancestor's chunk, mirroring `compile_super_call`'s walk.
      if recv_ty.is_a?( Frontend::NominalType ) && recv_ty.kind.object? && expr.name == "finalize" && expr.args.empty?
        self_reg = compile_expr( expr.receiver )
        return compile_static_finalize_call( recv_ty.name, self_reg )
      end

      if recv_ty.is_a?( Frontend::NominalType ) && ( recv_ty.kind.struct? || recv_ty.kind.object? )
        self_reg  = compile_expr( expr.receiver )
        arg_regs  = expr.args.map { |a| compile_expr( a ) }
        arg_types = expr.args.map { |a| a.resolved_type || Frontend::Type::UNKNOWN }
        if recv_ty.kind.struct?
          return compile_struct_call( recv_ty, expr.name, self_reg, arg_regs, arg_types )
        else
          return compile_virtual_call( recv_ty, expr.name, self_reg, arg_regs, arg_types )
        end
      end

      # A primitive receiver dispatches through its reopened builtin
      # (`struct Int … end` in the Core) : direct `CALL`, `self` = 1 slot.
      if info = primitive_owner( recv_ty, expr.name )
        self_reg  = compile_expr( expr.receiver )
        arg_regs  = expr.args.map { |a| compile_expr( a ) }
        arg_types = expr.args.map { |a| a.resolved_type || Frontend::Type::UNKNOWN }
        return compile_primitive_call( info, expr.name, self_reg, arg_regs, arg_types )
      end

      raise "internal: instance method dispatch is not yet lowered (Phase 4) : ##{expr.name}"
    end

    # Direct (non-virtual) call to `recv_ty#method_name` — the only dispatch
    # a struct value needs. `self` is placed by value (N slots, a real copy)
    # ahead of the declared arguments, matching the `base`-is-both-return-
    # and-arg-window convention used everywhere else (`compile_call`,
    # `compile_class_new`, `compile_struct_new`).
    private def compile_struct_call( recv_ty : Frontend::NominalType, method_name : String,
                                      self_reg : Int32, arg_regs : Array( Int32 ),
                                      arg_types : Array( Frontend::Type ) ) : Int32
      mangled    = "#{recv_ty.name}##{method_name}"
      idx        = @func_index[ mangled ]
      msig       = @types[ recv_ty.name ].methods[ method_name ]
      self_slots = slot_count( recv_ty )
      arg_slots  = arg_types.map { |t| slot_count( t ) }
      total      = self_slots + arg_slots.sum
      ret_slots  = slot_count( msig.ret )

      base = alloc_block( Math.max( 1 + total, ret_slots ) )
      place_value( base + 1, self_reg, self_slots )
      offset = 1 + self_slots
      arg_regs.each_with_index do |ar, i|
        n = arg_slots[ i ]
        place_value( base + offset, ar, n )
        offset += n
      end
      emit_abc( IR::Opcode::CALL, base, idx, total )
      base
    end

    # Static (never virtual) call to `type_name`'s own `finalize`, or the
    # nearest ancestor's if `type_name` declares none — same chain walk as
    # `compile_super_call`, starting at the receiver's own type instead of
    # its superclass. `finalize` takes no args : `self` is the only slot.
    private def compile_static_finalize_call( type_name : String, self_reg : Int32 ) : Int32
      idx  = nil.as( Int32? )
      cur  = type_name
      while cur && ( info = @types[ cur ]? )
        if i = @func_index[ "#{cur}#finalize" ]?
          idx = i
          break
        end
        cur = info.superclass
      end
      raise "internal: unresolved finalize #{type_name}#finalize" if idx.nil?

      base = alloc_block( 2 )
      place_value( base + 1, self_reg, 1 )
      emit_abc( IR::Opcode::CALL, base, idx, 1 )
      base
    end

    # `TypeInfo` backing a method call on a primitive receiver : the reopened
    # builtin is looked up under the exact width first (`Int32`), then the
    # inferred family (`Int`). Nil when no reopening declares the method.
    private def primitive_owner( recv_ty : Frontend::Type?, method_name : String ) : Frontend::TypeInfo?
      return nil unless recv_ty
      recv_ty.reopen_names.each do |owner|
        info = @types[ owner ]?
        return info if info && info.methods.has_key?( method_name )
      end
      nil
    end

    # Direct call to a reopened primitive's method — same windowed convention
    # as `compile_struct_call`, with `self` always exactly 1 slot.
    private def compile_primitive_call( info : Frontend::TypeInfo, method_name : String,
                                         self_reg : Int32, arg_regs : Array( Int32 ),
                                         arg_types : Array( Frontend::Type ) ) : Int32
      mangled   = "#{info.name}##{method_name}"
      idx       = @func_index[ mangled ]
      msig      = info.methods[ method_name ]
      arg_slots = arg_types.map { |t| slot_count( t ) }
      total     = 1 + arg_slots.sum
      ret_slots = slot_count( msig.ret )

      base = alloc_block( Math.max( 1 + total, ret_slots ) )
      place_value( base + 1, self_reg, 1 )
      offset = 2
      arg_regs.each_with_index do |ar, i|
        n = arg_slots[ i ]
        place_value( base + offset, ar, n )
        offset += n
      end
      emit_abc( IR::Opcode::CALL, base, idx, total )
      base
    end

    # `Module.method( args )` and unqualified module-internal calls. A module is
    # a compile-time namespace with no instances, so this lowers to a direct
    # `CALL` of the `Module#method` chunk with no `self` — arguments start at
    # `base + 1`, exactly like a free-function call.
    private def compile_static_call( owner_name : String, method_name : String,
                                     arg_regs : Array( Int32 ), arg_types : Array( Frontend::Type ) ) : Int32
      mangled   = "#{owner_name}##{method_name}"
      idx       = @func_index[ mangled ]
      msig      = @types[ owner_name ].methods[ method_name ]
      arg_slots = arg_types.map { |t| slot_count( t ) }
      total     = arg_slots.sum
      ret_slots = slot_count( msig.ret )

      base   = alloc_block( Math.max( 1 + total, ret_slots ) )
      offset = 1
      arg_regs.each_with_index do |ar, i|
        n = arg_slots[ i ]
        place_value( base + offset, ar, n )
        offset += n
      end
      emit_abc( IR::Opcode::CALL, base, idx, total )
      base
    end

    # Emits the up-front store of a module `@@var`'s default value into its
    # global slot (called from `BytecodeCompiler#compile_main`).
    def emit_global_init( slot : Int32, value_reg : Int32 ) : Nil
      emit_abx( IR::Opcode::STORE_GLOBAL, value_reg, slot )
    end

    # Walks own methods, then the superclass chain, then mixins — the same
    # resolution order `Semantic::TypeChecker#find_method` uses — to find the
    # signature backing a virtual or self-dispatched call.
    private def find_method( type_name : String, name : String ) : Frontend::FuncSig?
      info = @types[ type_name ]?
      return nil unless info
      if sig = info.methods[ name ]?
        return sig
      end
      if sup = info.superclass
        if sig = find_method( sup, name )
          return sig
        end
      end
      info.mixins.each do |m|
        if sig = find_method( m, name )
          return sig
        end
      end
      nil
    end

    private def nominal_of( info : Frontend::TypeInfo ) : Frontend::NominalType
      kind = info.kind.struct? ? Frontend::TypeKind::Struct : Frontend::TypeKind::Object
      Frontend::NominalType.new( info.name, kind, info.type_id, info.layout, info.reg_layout )
    end

    # True virtual dispatch (architecture #7.3) : `method_name`'s slot index
    # is resolved once, statically, from the receiver's *static* type
    # (`vtable_layout` is copy-forwarded down the whole hierarchy, so every
    # class that can reach this method agrees on its slot number) — but
    # which chunk that slot holds is only known at runtime, from the
    # receiver's *actual* class (`CALL_METHOD`'s VM handler). The receiver
    # is always exactly 1 slot (a class reference never flattens, unlike a
    # struct value), placed first in the arg window like every other call.
    private def compile_virtual_call( recv_ty : Frontend::NominalType, method_name : String,
                                       self_reg : Int32, arg_regs : Array( Int32 ),
                                       arg_types : Array( Frontend::Type ) ) : Int32
      vtidx = @types[ recv_ty.name ]?.try( &.vtable_layout[ method_name ]? )
      raise "internal: unknown virtual method #{recv_ty.name}##{method_name}" if vtidx.nil?

      msig = find_method( recv_ty.name, method_name )
      raise "internal: unresolved method signature #{recv_ty.name}##{method_name}" if msig.nil?

      arg_slots = arg_types.map { |t| slot_count( t ) }
      total     = 1 + arg_slots.sum
      ret_slots = slot_count( msig.ret )

      base = alloc_block( Math.max( 1 + total, ret_slots ) )
      place_value( base + 1, self_reg, 1 )
      offset = 2
      arg_regs.each_with_index do |ar, i|
        n = arg_slots[ i ]
        place_value( base + offset, ar, n )
        offset += n
      end
      emit_abc( IR::Opcode::CALL_METHOD, base, vtidx, total )
      base
    end

    # `super( args )` : a *static* call to the nearest ancestor's chunk — never
    # virtual (a virtual dispatch would land right back on the caller's own
    # override and loop). The walk mirrors `BytecodeCompiler#build_vtable`'s
    # inherited-slot resolution : the ancestor's chunk exists under its own
    # name whether the method is written on it directly or adopted from a
    # mixin. `self` is passed through unchanged (1 slot : classes only —
    # Semantic rejects `super` everywhere else).
    private def compile_super_call( expr : Frontend::SuperCall ) : Int32
      owner  = @self_owner.not_nil!
      method = @method_name.not_nil!

      idx  = nil.as( Int32? )
      msig = nil.as( Frontend::FuncSig? )
      cur  = owner.superclass
      while cur && ( info = @types[ cur ]? )
        if method == "initialize"
          if sig = info.initializers[ expr.args.size ]? || info.initializer
            if i = @func_index[ init_chunk_name( info, expr.args.size ) ]?
              idx  = i
              msig = sig
              break
            end
          end
        elsif i = @func_index[ "#{cur}##{method}" ]?
          idx  = i
          msig = find_method( cur, method )
          break
        end
        cur = info.superclass
      end
      if idx.nil? || msig.nil?
        raise "internal: unresolved super #{owner.name}##{method} (should be rejected by Semantic)"
      end

      self_reg  = @scope[ "self" ]
      arg_regs  = expr.args.map { |a| compile_expr( a ) }
      arg_slots = expr.args.map { |a| slot_count( a.resolved_type || Frontend::Type::UNKNOWN ) }
      total     = 1 + arg_slots.sum
      ret_slots = slot_count( msig.ret )

      base = alloc_block( Math.max( 1 + total, ret_slots ) )
      place_value( base + 1, self_reg, 1 )
      offset = 2
      arg_regs.each_with_index do |ar, i|
        n = arg_slots[ i ]
        place_value( base + offset, ar, n )
        offset += n
      end
      emit_abc( IR::Opcode::CALL, base, idx, total )
      base
    end

    #------------------------------------------------------------------------------------
    # Object construction (Phase 2 : no vtables yet, see architecture #1.B for
    # why structs need no allocation at all).

    private def compile_constructor_call( info : Frontend::TypeInfo, args : Array( Frontend::AExpr ) ) : Int32
      info.kind.struct? ? compile_struct_new( info, args ) : compile_class_new( info, args )
    end

    # A struct has no heap indirection (architecture #1.B) : its value *is*
    # the register range. So an explicit `initialize` can't mutate a
    # pre-existing `self` the way a class constructor does — it runs in its
    # own callee frame and must hand the finished value back through `RET`.
    # `compile_method` special-cases a struct's `initialize` to return `self`
    # (all of its slots) instead of the body's own result; the call site here
    # just has to land that N-slot return exactly where the constructed value
    # should live — the same `base`-is-both-return-and-arg-window convention
    # `compile_class_new`/`compile_call` already use.
    private def compile_struct_new( info : Frontend::TypeInfo, args : Array( Frontend::AExpr ) ) : Int32
      if init = ( info.initializers[ args.size ]? || info.initializer )
        idx      = @func_index[ init_chunk_name( info, args.size ) ]
        self_slots = info.reg_layout.try( &.total_size ) || 1
        arg_slot_counts = args.each_index.map { |i| slot_count( init.params[ i ]? || Frontend::Type::UNKNOWN ) }.to_a
        total    = self_slots + arg_slot_counts.sum
        base     = alloc_block( 1 + total )
        offset   = 1 + self_slots
        args.each_with_index do |a, i|
          ar = compile_expr( a )
          n  = arg_slot_counts[ i ]
          place_value( base + offset, ar, n )
          offset += n
        end
        emit_abc( IR::Opcode::CALL, base, idx, total )
        return base
      end

      layout = info.reg_layout.not_nil!
      base   = alloc_block( layout.total_size )
      emit_abx( IR::Opcode::NEW_STRUCT, base, layout.total_size )

      offset = 0
      layout.fields.each_with_index do |f, i|
        arg_reg = compile_expr( args[ i ] )
        n       = slot_count( f.type )
        place_value( base + offset, arg_reg, n )
        offset += n
      end
      base
    end

    private def compile_class_new( info : Frontend::TypeInfo, args : Array( Frontend::AExpr ) ) : Int32
      obj = alloc
      emit_abx( IR::Opcode::INIT_OBJ, obj, info.type_id )

      if provider = find_initializer_owner( info )
        init    = provider.initializers[ args.size ]? || provider.initializer.not_nil!
        idx     = @func_index[ init_chunk_name( provider, args.size ) ]
        arg_slot_counts = args.each_index.map { |i| slot_count( init.params[ i ]? || Frontend::Type::UNKNOWN ) }.to_a
        total   = 1 + arg_slot_counts.sum
        # `window` (== A) is the CALL's return-value landing spot : args
        # (self first, then declared params) start at `window + 1`, same
        # convention as a free-function call (see `compile_call`).
        window  = alloc_block( 1 + total )
        place_value( window + 1, obj, 1 )
        offset = 2
        args.each_with_index do |a, i|
          ar = compile_expr( a )
          n  = arg_slot_counts[ i ]
          place_value( window + offset, ar, n )
          offset += n
        end
        emit_abc( IR::Opcode::CALL, window, idx, total )
      end

      obj
    end

    # A class with no `initialize` of its own inherits its nearest
    # ancestor's — that ancestor's own `#initialize` chunk is what actually
    # exists (`collect_method_jobs` only compiles a type's *own* methods),
    # and it's safe to run against `obj` regardless of `obj`'s real subtype
    # since inherited fields share the same slot offsets (layout prefix
    # sharing, architecture #1.A.3).
    # Chunk key for a type's `initialize` overload : a lone constructor keeps
    # the historical plain `Type#initialize` name, multiple overloads are
    # arity-mangled `Type#initialize/<arity>` (see `TypeCollector`).
    private def init_chunk_name( info : Frontend::TypeInfo, arity : Int32 ) : String
      key = info.initializers.size > 1 ? "initialize/#{arity}" : "initialize"
      "#{info.name}##{key}"
    end

    private def find_initializer_owner( info : Frontend::TypeInfo ) : Frontend::TypeInfo?
      return info if info.initializer
      sup = info.superclass.try { |s| @types[ s ]? }
      sup.try { |s| find_initializer_owner( s ) }
    end

    private def place_value( dest : Int32, src : Int32, n : Int32 ) : Nil
      if n <= 1
        emit_abc( IR::Opcode::MOVE, dest, src, 0 )
      else
        emit_abc( IR::Opcode::COPY_BLOCK, dest, src, n )
      end
    end

    private def find_field( type_name : String, name : String ) : Frontend::FieldSlot?
      @types[ type_name ]?.try( &.layout ).try( &.field?( name ) )
    end

    private def field_slot_index( type_name : String, name : String ) : Int32
      @types[ type_name ]?.try( &.reg_layout ).try( &.field?( name ) ).try( &.offset ) || 0
    end

    private def compile_binary( expr : Frontend::BinaryOp ) : Int32
      case expr.op
      when .and? then return compile_and( expr )
      when .or?  then return compile_or( expr )
      end

      # Pointer arithmetic (+ / -)
      lt = expr.left.resolved_type
      if lt && lt.pointer? && ( expr.op.plus? || expr.op.minus? )
        lreg = compile_expr( expr.left )
        rreg = compile_expr( expr.right )
        scale = lt.pointee.byte_size
        scaled_reg = rreg
        if scale > 1
          scaled_reg = alloc
          scale_const = alloc
          scale_idx = add_const( IR::Value.int( scale.to_i64 ) )
          emit_abx( IR::Opcode::LOAD_CONST, scale_const, scale_idx )
          emit_abc( IR::Opcode::MUL_INT, scaled_reg, rreg, scale_const )
        end
        dest = alloc
        op = expr.op.plus? ? IR::Opcode::PTR_ADD : IR::Opcode::PTR_SUB
        emit_abc( op, dest, lreg, scaled_reg )
        return dest
      end

      lreg = compile_expr( expr.left )
      rreg = compile_expr( expr.right )

      # `Float#to_string`'s runtime shim (`Vm#to_string_value`, the `TO_STRING`
      # opcode) is a real Core `String` instance at runtime even though its
      # *static* type stays the legacy `Type::STR` (no `NominalType` — see
      # `TypeChecker#string_type`) : route `+` between two such legacy-typed
      # operands to the same `String#+` dispatch as the nominal case, keyed
      # off the Core's own registered `String` type rather than either
      # operand's static type.
      if expr.op.plus? && expr.left.resolved_type.try( &.kind.str? ) && expr.right.resolved_type.try( &.kind.str? ) &&
         ( info = @types[ "String" ]? )
        return compile_virtual_call( nominal_of( info ), "+", lreg, [ rreg ], [ Frontend::Type::UNKNOWN ] )
      end

      if ( op_name = operator_method_name( expr.op ) )
        lt = expr.left.resolved_type
        if lt.is_a?( Frontend::NominalType ) && ( lt.kind.struct? || lt.kind.object? ) && @types[ lt.name ]?.try( &.methods[ op_name ]? )
          rt     = expr.right.resolved_type || Frontend::Type::UNKNOWN
          result = lt.kind.struct? ? compile_struct_call( lt, op_name, lreg, [ rreg ], [ rt ] ) :
                                      compile_virtual_call( lt, op_name, lreg, [ rreg ], [ rt ] )
          if expr.op.bang_eq? || expr.op.not_match_op?
            neg = alloc
            emit_abc( IR::Opcode::NOT, neg, result, 0 )
            return neg
          end
          return result
        elsif ( info = primitive_owner( lt, op_name ) )
          rt     = expr.right.resolved_type || Frontend::Type::UNKNOWN
          result = compile_primitive_call( info, op_name, lreg, [ rreg ], [ rt ] )
          if expr.op.bang_eq? || expr.op.not_match_op?
            neg = alloc
            emit_abc( IR::Opcode::NOT, neg, result, 0 )
            return neg
          end
          return result
        end
      end

      is_f64 = numeric_float?( expr.left )
      dest   = alloc
      op     = binary_opcode( expr.op, is_f64 )
      if ( expr.op.eq_eq? || expr.op.bang_eq? ) &&
         expr.left.resolved_type.try( &.integer? ) && expr.right.resolved_type.try( &.integer? )
        op = expr.op.eq_eq? ? IR::Opcode::EQ_INT : IR::Opcode::NE_INT
      end
      emit_abc( op, dest, lreg, rreg )

      if expr.op.amp_plus? || expr.op.amp_minus? || expr.op.amp_star? || expr.op.amp_star_star?
        if ( t = expr.resolved_type ) && t.integer? && t.int_bit_width < 64
          emit_abx( IR::Opcode::CONV_INT, dest, t.int_bit_width )
        end
      end

      dest
    end

    private def compile_range_includes( expr : Frontend::MethodCall ) : Int32
      range = expr.receiver.as( Frontend::RangeExpr )
      val   = expr.args.first

      val_reg = compile_expr( val )

      from_reg = compile_expr( range.from )
      to_reg   = compile_expr( range.to )

      left_res = alloc
      is_f64   = numeric_float?( range.from )
      op_left  = is_f64 ? IR::Opcode::GE_F64 : IR::Opcode::GE_INT
      emit_abc( op_left, left_res, val_reg, from_reg )

      right_res = alloc
      op_right  = if range.exclusive
                    is_f64 ? IR::Opcode::LT_F64 : IR::Opcode::LT_INT
                  else
                    is_f64 ? IR::Opcode::LE_F64 : IR::Opcode::LE_INT
                  end
      emit_abc( op_right, right_res, val_reg, to_reg )

      dest = alloc
      emit_abc( IR::Opcode::MOVE, dest, left_res, 0 )
      skip = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, dest )
      emit_abc( IR::Opcode::MOVE, dest, right_res, 0 )
      patch_jump( skip, here )
      dest
    end

    private def compile_and( expr : Frontend::BinaryOp ) : Int32
      dest = alloc
      lreg = compile_expr( expr.left )
      emit_abc( IR::Opcode::MOVE, dest, lreg, 0 )
      skip = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, dest )
      rreg = compile_expr( expr.right )
      emit_abc( IR::Opcode::MOVE, dest, rreg, 0 )
      patch_jump( skip, here )
      dest
    end

    private def compile_or( expr : Frontend::BinaryOp ) : Int32
      dest = alloc
      lreg = compile_expr( expr.left )
      emit_abc( IR::Opcode::MOVE, dest, lreg, 0 )
      neg  = alloc
      emit_abc( IR::Opcode::NOT, neg, dest, 0 )
      skip = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, neg )
      rreg = compile_expr( expr.right )
      emit_abc( IR::Opcode::MOVE, dest, rreg, 0 )
      patch_jump( skip, here )
      dest
    end

    private def compile_unary( expr : Frontend::UnaryOp ) : Int32
      if expr.op.amp?
        dest = alloc
        target = expr.operand
        case target
        when Frontend::Ident
          name = target.name
          if slot = @scope[ name ]?
            emit_abc( IR::Opcode::ADDR_LOCAL, dest, slot, 0 )
          else
            raise "internal: address of undefined variable #{name}"
          end
        when Frontend::InstanceVar
          self_reg = @scope[ "self" ]
          owner = @self_owner.not_nil!
          field_slot = field_slot_index( owner.name, target.name )
          if owner.kind.struct?
            emit_abc( IR::Opcode::ADDR_LOCAL, dest, self_reg + field_slot, 0 )
          else
            emit_abc( IR::Opcode::ADDR_FIELD, dest, self_reg, field_slot )
          end
        when Frontend::MemberAccess
          recv_ty = target.receiver.resolved_type.not_nil!
          if recv_ty.kind.struct?
            slot = compile_expr( target )
            emit_abc( IR::Opcode::ADDR_LOCAL, dest, slot, 0 )
          else
            raise "internal: expected nominal type for member access address-of" unless recv_ty.is_a?( Frontend::NominalType )
            recv_reg = compile_expr( target.receiver )
            field_slot = field_slot_index( recv_ty.name, target.name )
            emit_abc( IR::Opcode::ADDR_FIELD, dest, recv_reg, field_slot )
          end
        else
          raise "internal: unsupported address-of target: #{target.class}"
        end
        return dest
      end

      oreg = compile_expr( expr.operand )
      dest = alloc
      case expr.op
      when .minus?
        op = numeric_float?( expr.operand ) ? IR::Opcode::NEG_F64 : IR::Opcode::NEG_INT
        emit_abc( op, dest, oreg, 0 )
      when .tilde?
        emit_abc( IR::Opcode::NOT_INT, dest, oreg, 0 )
      when .star?
        width = IR::PtrWidth.for( expr.resolved_type.not_nil! )
        emit_abc( IR::Opcode::LOAD_PTR, dest, oreg, width.value )
      else # bang / not
        emit_abc( IR::Opcode::NOT, dest, oreg, 0 )
      end
      dest
    end

    private def compile_call( expr : Frontend::Call ) : Int32
      name = expr.callee.as( Frontend::Ident ).name

      # Unqualified call inside a method body : an own/inherited/mixin
      # method on `self` takes priority over a free function of the same
      # name (mirrors `Semantic::TypeChecker#infer_call`). Routed through
      # the same dispatch each receiver kind otherwise uses externally —
      # virtual for a class (so an inherited method calling another
      # overridable method still late-binds to the actual subclass), direct
      # for a struct.
      if ( owner = @self_owner ) && !@scope.has_key?( name ) && find_method( owner.name, name )
        arg_regs  = expr.args.map { |a| compile_expr( a ) }
        arg_types = expr.args.map { |a| a.resolved_type || Frontend::Type::UNKNOWN }
        if owner.kind.module?
          return compile_static_call( owner.name, name, arg_regs, arg_types )
        end
        self_reg = @scope[ "self" ]
        if owner.kind.struct?
          return compile_struct_call( nominal_of( owner ), name, self_reg, arg_regs, arg_types )
        else
          return compile_virtual_call( nominal_of( owner ), name, self_reg, arg_regs, arg_types )
        end
      end

      sig  = @signatures[ name ]

      arg_regs   = expr.args.map { |a| compile_expr( a ) }
      arg_slots  = expr.args.map { |a| slot_count( a.resolved_type || Frontend::Type::UNKNOWN ) }
      total_args = arg_slots.sum
      ret_slots  = slot_count( sig.ret )

      base = alloc_block( Math.max( 1 + total_args, ret_slots ) )
      offset = 1
      arg_regs.each_with_index do |ar, i|
        n = arg_slots[ i ]
        place_value( base + offset, ar, n )
        offset += n
      end

      if sig.extern
        emit_abc( IR::Opcode::CALL_NATIVE, base, @natives.intern( sig.lib, name ), total_args )
      else
        emit_abc( IR::Opcode::CALL, base, @func_index[ name ], total_args )
      end

      base
    end

    private def compile_if( expr : Frontend::IfExpr ) : Int32
      end_jumps = [] of Int32
      n         = slot_count( expr.resolved_type || Frontend::Type::UNKNOWN )
      dest      = alloc_block( n )

      creg    = compile_expr( expr.cond )
      to_next = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, creg )
      then_reg = compile_body( expr.then_b )
      place_value( dest, then_reg, n )
      end_jumps << emit_jump_placeholder( IR::Opcode::JMP, 0 )
      patch_jump( to_next, here )

      expr.elsifs.each do |cond, body|
        creg    = compile_expr( cond )
        to_next = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, creg )
        body_reg = compile_body( body )
        place_value( dest, body_reg, n )
        end_jumps << emit_jump_placeholder( IR::Opcode::JMP, 0 )
        patch_jump( to_next, here )
      end

      if eb = expr.else_b
        else_reg = compile_body( eb )
        place_value( dest, else_reg, n )
      else
        emit_abc( IR::Opcode::LOAD_NIL, dest, 0, 0 )
      end

      end_jumps.each { |j| patch_jump( j, here ) }

      dest
    end

    private def compile_while( expr : Frontend::WhileExpr ) : Int32
      # Allocated up front (rather than after the body, as a plain `while`
      # with no `break value` would need) so `compile_break` has a register
      # to write a break-value into regardless of where in the body it fires.
      dest = alloc
      emit_abc( IR::Opcode::LOAD_NIL, dest, 0, 0 )

      top    = here
      creg   = compile_expr( expr.cond )
      to_end = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, creg )

      @loop_stack.push( LoopCtx.new( top, dest ) )
      compile_body( expr.body )
      ctx = @loop_stack.pop

      emit_abx( IR::Opcode::JMP, 0, top )
      patch_jump( to_end, here )
      ctx.break_jumps.each { |j| patch_jump( j, here ) }

      dest
    end

    # `break` / `break value` : an optional value first (Ruby/Crystal-style —
    # becomes the enclosing `while`'s own result), then an unconditional jump
    # patched to land just past the loop once its end address is known.
    private def compile_break( expr : Frontend::BreakExpr ) : Int32
      ctx = @loop_stack.last
      if v = expr.value
        vreg = compile_expr( v )
        place_value( ctx.result_reg, vreg, 1 )
      end
      ctx.break_jumps << emit_jump_placeholder( IR::Opcode::JMP, 0 )
      ctx.result_reg
    end

    # `next` : any value expression is still compiled for its side effects
    # (matches evaluating a statement that's simply never used), then jumps
    # straight back to the loop's condition re-check — no value it could
    # carry is observable anywhere (`while` never yields per-iteration).
    private def compile_next( expr : Frontend::NextExpr ) : Int32
      ctx = @loop_stack.last
      if v = expr.value
        compile_expr( v )
      end
      emit_abx( IR::Opcode::JMP, 0, ctx.continue_target )
      ctx.result_reg
    end

    private def compile_return( expr : Frontend::ReturnExpr ) : Int32
      reg, slots = if v = expr.value
        { compile_expr( v ), slot_count( v.resolved_type || Frontend::Type::UNKNOWN ) }
      else
        r = alloc
        emit_abc( IR::Opcode::LOAD_NIL, r, 0, 0 )
        { r, 1 }
      end
      emit_scope_cleanup
      emit_ret( reg, slots )
      reg
    end

    private def numeric_float?( expr : Frontend::AExpr ) : Bool
      ( t = expr.resolved_type ) ? t.float? : false
    end

    # Mirrors `TypeChecker#operator_method`'s candidate op set : the only
    # operators eligible for a struct/class operator-overload method lookup.
    private def operator_method_name( op : Frontend::TokenKind ) : String?
      case op
      when .plus?             then "+"
      when .minus?            then "-"
      when .star?             then "*"
      when .slash?            then "/"
      when .percent?          then "%"
      when .eq_eq?, .bang_eq? then "=="
      when .match_op?         then "=~"
      when .not_match_op?     then "=~"
      when .eq_eq_eq?         then "==="
      else                         nil
      end
    end

    private def binary_opcode( kind : Frontend::TokenKind, f64 : Bool ) : IR::Opcode
      case kind
      when .plus?                       then f64 ? IR::Opcode::ADD_F64 : IR::Opcode::ADD_INT
      when .minus?                      then f64 ? IR::Opcode::SUB_F64 : IR::Opcode::SUB_INT
      when .star?                       then f64 ? IR::Opcode::MUL_F64 : IR::Opcode::MUL_INT
      when .slash?                      then f64 ? IR::Opcode::DIV_F64 : IR::Opcode::DIV_INT
      when .percent?                    then IR::Opcode::MOD_INT
      when .amp_plus?                   then f64 ? IR::Opcode::ADD_F64 : IR::Opcode::ADD_INT
      when .amp_minus?                  then f64 ? IR::Opcode::SUB_F64 : IR::Opcode::SUB_INT
      when .amp_star?                   then f64 ? IR::Opcode::MUL_F64 : IR::Opcode::MUL_INT
      when .slash_slash?                then IR::Opcode::IDIV_INT
      when .star_star?, .amp_star_star? then IR::Opcode::POW_INT
      when .amp?                        then IR::Opcode::AND_INT
      when .pipe?                       then IR::Opcode::OR_INT
      when .caret?                      then IR::Opcode::XOR_INT
      when .lt_lt?                      then IR::Opcode::SHL_INT
      when .gt_gt?                      then IR::Opcode::SHR_INT
      when .lt?                         then f64 ? IR::Opcode::LT_F64 : IR::Opcode::LT_INT
      when .lt_eq?                      then f64 ? IR::Opcode::LE_F64 : IR::Opcode::LE_INT
      when .gt?                         then f64 ? IR::Opcode::GT_F64 : IR::Opcode::GT_INT
      when .gt_eq?                      then f64 ? IR::Opcode::GE_F64 : IR::Opcode::GE_INT
      when .spaceship?                  then IR::Opcode::CMP_INT
      when .eq_eq?, .eq_eq_eq?          then IR::Opcode::EQ
      when .bang_eq?                    then IR::Opcode::NE
      else
        raise "internal: unhandled binary opcode #{kind}"
      end
    end
    #------------------------------------------------------------------------------------

  end


end
