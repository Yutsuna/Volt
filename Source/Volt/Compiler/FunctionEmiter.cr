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
                    @self_owner : Frontend::TypeInfo? = nil )
      @chunk    = IR::Chunk.new( name, arity )
      @scope    = {} of String => Int32
      @next_reg = 0
      @max_reg  = 0
      @scopes_resource_registers = [ [] of Tuple( Int32, Int32 ) ]
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
        emit_abx( IR::Opcode::DROP_SCOPE, 0, table_idx )

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

    def compile_body( nodes : Array( Frontend::ANode ) ) : Int32
      last = -1
      nodes.each do |node|
        last = compile_expr( node.as( Frontend::AExpr ) )
      end
      if last < 0
        last = alloc
        emit_abc( IR::Opcode::LOAD_NIL, last, 0, 0 )
      end
      last
    end

    def compile_expr( expr : Frontend::AExpr ) : Int32
      case expr
      when Frontend::IntLit     then const_reg( IR::Value.int( expr.value ) )
      when Frontend::FloatLit   then const_reg( IR::Value.float( expr.value ) )
      when Frontend::StringLit  then const_reg( IR::Value.str( expr.value ) )
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
      when Frontend::MemberAccess then compile_member_access( expr )
      when Frontend::SelfExpr     then @scope[ "self" ]
      when Frontend::Assign       then compile_assign( expr )
      when Frontend::BinaryOp     then compile_binary( expr )
      when Frontend::UnaryOp      then compile_unary( expr )
      when Frontend::Call         then compile_call( expr )
      when Frontend::MethodCall   then compile_method_call( expr )
      when Frontend::IfExpr     then compile_if( expr )
      when Frontend::WhileExpr  then compile_while( expr )
      when Frontend::ReturnExpr then compile_return( expr )
      when Frontend::RaiseExpr
        val_reg = compile_expr( expr.value )
        emit_abc( IR::Opcode::RAISE, val_reg, 0, 0 )
        val_reg
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
      when Frontend::MemberAccess then compile_assign_member( expr, target )
      else
        raise "internal: unlowerable assignment target #{expr.target.class}"
      end
    end

    private def compile_assign_ident( expr : Frontend::Assign, target : Frontend::Ident ) : Int32
      name      = target.name
      value_reg = compile_expr( expr.value )
      if home = @scope[ name ]?
        place_value( home, value_reg, slot_count( target.resolved_type || Frontend::Type::UNKNOWN ) )
        home
      else
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
      value_reg = compile_expr( expr.value )
      n         = slot_count( target.resolved_type || Frontend::Type::UNKNOWN )

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
        reg
      elsif sig = @signatures[ expr.name ]?
        base = alloc_block( Math.max( 1, slot_count( sig.ret ) ) )
        if sig.extern
          emit_abc( IR::Opcode::CALL_NATIVE, base, @natives.intern( sig.lib, expr.name ), 0 )
        else
          emit_abc( IR::Opcode::CALL, base, @func_index[ expr.name ], 0 )
        end
        base
      else
        raise "internal: unbound identifier #{expr.name}"
      end
    end

    private def compile_instance_var( expr : Frontend::InstanceVar ) : Int32
      owner    = @self_owner.not_nil!
      self_reg = @scope[ "self" ]
      slot     = field_slot_index( owner.name, expr.name )
      if owner.kind.struct?
        self_reg + slot
      else
        dest = alloc
        emit_abc( IR::Opcode::LOAD_FIELD, dest, self_reg, slot )
        dest
      end
    end

    private def compile_member_access( expr : Frontend::MemberAccess ) : Int32
      return compile_to_s( expr.receiver ) if expr.name == "to_s"

      recv_ty = expr.receiver.resolved_type
      raise "internal: member access on non-object type" unless recv_ty.is_a?( Frontend::NominalType )
      recv_reg = compile_expr( expr.receiver )

      unless find_field( recv_ty.name, expr.name )
        # A struct has no polymorphism (no subclassing, no mixins), so a
        # bare zero-arg method call (`v.magnitude`, reclassified from field
        # access by the parser/Semantic) resolves to a plain direct call —
        # no vtable needed, unlike the still-deferred class case below.
        if recv_ty.kind.struct? && @types[ recv_ty.name ]?.try( &.methods[ expr.name ]? )
          return compile_struct_call( recv_ty, expr.name, recv_reg, [] of Int32, [] of Frontend::Type )
        end
        raise "internal: zero-arg method calls are not yet lowered (Phase 4) : #{recv_ty.name}##{expr.name}"
      end

      slot = field_slot_index( recv_ty.name, expr.name )
      if recv_ty.kind.struct?
        recv_reg + slot
      else
        dest = alloc
        emit_abc( IR::Opcode::LOAD_FIELD, dest, recv_reg, slot )
        dest
      end
    end

    private def compile_to_s( receiver : Frontend::AExpr ) : Int32
      r    = compile_expr( receiver )
      dest = alloc
      emit_abc( IR::Opcode::TO_STRING, dest, r, 0 )
      dest
    end

    private def compile_method_call( expr : Frontend::MethodCall ) : Int32
      if expr.name == "includes?" && expr.receiver.is_a?( Frontend::RangeExpr )
        return compile_range_includes( expr )
      end
      return compile_to_s( expr.receiver ) if expr.name == "to_s"

      if ( recv = expr.receiver ).is_a?( Frontend::Ident ) && !@scope.has_key?( recv.name ) &&
         ( info = @types[ recv.name ]? ) && expr.name == "new"
        return compile_constructor_call( info, expr.args )
      end

      # Structs have no subclassing/mixins to dispatch through — a method
      # call on a struct value always resolves to exactly one chunk, so it
      # compiles straight to a direct `CALL` (unlike the class case below,
      # which needs the vtable machinery of Phase 4).
      recv_ty = expr.receiver.resolved_type
      if recv_ty.is_a?( Frontend::NominalType ) && recv_ty.kind.struct?
        self_reg  = compile_expr( expr.receiver )
        arg_regs  = expr.args.map { |a| compile_expr( a ) }
        arg_types = expr.args.map { |a| a.resolved_type || Frontend::Type::UNKNOWN }
        return compile_struct_call( recv_ty, expr.name, self_reg, arg_regs, arg_types )
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
      if init = info.initializer
        mangled  = "#{info.name}#initialize"
        idx      = @func_index[ mangled ]
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

      if init = info.initializer
        mangled = "#{info.name}#initialize"
        idx     = @func_index[ mangled ]
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

      lreg = compile_expr( expr.left )
      rreg = compile_expr( expr.right )

      if expr.op.plus? && expr.left.resolved_type.try( &.kind.str? ) && expr.right.resolved_type.try( &.kind.str? )
        dest = alloc
        emit_abc( IR::Opcode::CONCAT_STR, dest, lreg, rreg )
        return dest
      end

      if ( lt = expr.left.resolved_type ).is_a?( Frontend::NominalType ) && lt.kind.struct? &&
         ( op_name = operator_method_name( expr.op ) ) && @types[ lt.name ]?.try( &.methods[ op_name ]? )
        rt     = expr.right.resolved_type || Frontend::Type::UNKNOWN
        result = compile_struct_call( lt, op_name, lreg, [ rreg ], [ rt ] )
        if expr.op.bang_eq?
          neg = alloc
          emit_abc( IR::Opcode::NOT, neg, result, 0 )
          return neg
        end
        return result
      end

      is_f64 = numeric_float?( expr.left )
      dest   = alloc
      emit_abc( binary_opcode( expr.op, is_f64 ), dest, lreg, rreg )

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
      oreg = compile_expr( expr.operand )
      dest = alloc
      case expr.op
      when .minus?
        op = numeric_float?( expr.operand ) ? IR::Opcode::NEG_F64 : IR::Opcode::NEG_INT
        emit_abc( op, dest, oreg, 0 )
      when .tilde?
        emit_abc( IR::Opcode::NOT_INT, dest, oreg, 0 )
      else # bang / not
        emit_abc( IR::Opcode::NOT, dest, oreg, 0 )
      end
      dest
    end

    private def compile_call( expr : Frontend::Call ) : Int32
      name = expr.callee.as( Frontend::Ident ).name
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

      creg    = compile_expr( expr.cond )
      to_next = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, creg )
      compile_body( expr.then_b )
      end_jumps << emit_jump_placeholder( IR::Opcode::JMP, 0 )
      patch_jump( to_next, here )

      expr.elsifs.each do |cond, body|
        creg    = compile_expr( cond )
        to_next = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, creg )
        compile_body( body )
        end_jumps << emit_jump_placeholder( IR::Opcode::JMP, 0 )
        patch_jump( to_next, here )
      end

      if eb = expr.else_b
        compile_body( eb )
      end

      end_jumps.each { |j| patch_jump( j, here ) }

      dest = alloc
      emit_abc( IR::Opcode::LOAD_NIL, dest, 0, 0 )
      dest
    end

    private def compile_while( expr : Frontend::WhileExpr ) : Int32
      top    = here
      creg   = compile_expr( expr.cond )
      to_end = emit_jump_placeholder( IR::Opcode::JMP_IF_FALSE, creg )
      compile_body( expr.body )
      emit_abx( IR::Opcode::JMP, 0, top )
      patch_jump( to_end, here )

      dest = alloc
      emit_abc( IR::Opcode::LOAD_NIL, dest, 0, 0 )
      dest
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
      when .eq_eq?                      then IR::Opcode::EQ
      when .eq_eq_eq?                   then IR::Opcode::EQ_CASE
      when .match_op?                   then IR::Opcode::MATCH_STR
      when .not_match_op?               then IR::Opcode::NOT_MATCH_STR
      when .bang_eq?                    then IR::Opcode::NE
      else
        raise "internal: unhandled binary opcode #{kind}"
      end
    end
    #------------------------------------------------------------------------------------

  end


end
