require "./Unit"
require "./NativeTable"
require "./FunctionEmiter"

module Volt::Compiler


  class BytecodeCompiler
    @func_index : Hash( String, Int32 )

    #------------------------------------------------------------------------------------

    def initialize( @typed : Frontend::TypedProgram )
      @func_index   = {} of String => Int32
      @natives      = NativeTable.new
      @global_index = {} of String => Int32
      # (global slot, default-value expr, owning module) in declaration order.
      @global_inits = [] of { Int32, Frontend::AExpr?, String }
    end

    #------------------------------------------------------------------------------------

    def compile : Unit
      collect_globals
      @typed.functions.each_with_index { |fn, i| @func_index[ fn.name ] = i }

      method_jobs = collect_method_jobs
      next_idx    = @typed.functions.size
      method_jobs.each do |mangled, _owner, _decl, _sig|
        @func_index[ mangled ] = next_idx
        next_idx += 1
      end

      drop_fields_types = @typed.types.values.select do |info|
        info.kind.class? && info.layout.try( &.fields.any? { |f| droppable?( f ) } ) || false
      end
      drop_fields_types.each do |info|
        @func_index[ "#{info.name}#__drop_fields" ] = next_idx
        next_idx += 1
      end

      chunks = @typed.functions.map { |fn| compile_function( fn ) }
      chunks.concat( method_jobs.map { |mangled, owner, decl, sig| compile_method( mangled, owner, decl, sig ) } )
      chunks.concat( drop_fields_types.map { |info| compile_drop_fields( info ) } )
      chunks << compile_main

      Unit.new( chunks, chunks.size - 1, @natives.natives, build_classes, @global_index.size )
    end

    # Assigns each module `@@var` a process-global slot and records its default
    # initializer (emitted at the top of `main`), walking nested modules too.
    private def collect_globals : Nil
      @typed.program.nodes.each { |node| collect_module_globals( node ) if node.is_a?( Frontend::ModuleDecl ) }
    end

    private def collect_module_globals( mod : Frontend::ModuleDecl ) : Nil
      mod.body.each do |node|
        case node
        when Frontend::ClassVarDecl
          slot = @global_index.size
          @global_index[ "#{mod.name}::#{node.name}" ] = slot
          @global_inits << { slot, node.value, mod.name }
        when Frontend::ModuleDecl
          collect_module_globals( node )
        end
      end
    end

    #------------------------------------------------------------------------------------

    # A struct has no subclassing/mixins, so every method lowers straight to
    # a direct call (Phase 3) — every method is compiled eagerly.
    #
    # A class's own methods compile the same way (needed as vtable slot
    # targets), but a *mixin*'s methods are compiled once per *including*
    # class rather than once under the mixin's own name : a mixin body
    # references the includer's instance state (`@price` in `Taxable`, owned
    # by whichever concrete product includes it), and that state's field
    # offsets only make sense once resolved against a real layout — the
    # mixin itself has none. So each including class gets its own compiled
    # copy, mangled under its own name, exactly as if the method had been
    # written directly on that class (a class's own method of the same name
    # still wins and suppresses the adopted copy).
    private def collect_method_jobs : Array( { String, Frontend::TypeInfo, Frontend::FuncDecl, Frontend::FuncSig } )
      jobs = [] of { String, Frontend::TypeInfo, Frontend::FuncDecl, Frontend::FuncSig }
      @typed.types.each_value do |info|
        # A module's methods are static (no `self`) but still compile to
        # ordinary chunks, keyed `Module#method`, called directly.
        if info.kind.module?
          info.methods_ast.each do |mname, decl|
            next if decl.is_abstract
            jobs << { "#{info.name}##{mname}", info, decl, info.methods[ mname ] }
          end
          next
        end

        next unless info.kind.struct? || info.kind.class?

        info.methods_ast.each do |mname, decl|
          next if decl.is_abstract
          jobs << { "#{info.name}##{mname}", info, decl, info.methods[ mname ] }
        end

        next unless info.kind.class?
        info.mixins.each do |mixin_name|
          mixin_info = @typed.types[ mixin_name ]?
          next unless mixin_info
          mixin_info.methods_ast.each do |mname, mdecl|
            next if info.methods_ast.has_key?( mname )
            jobs << { "#{info.name}##{mname}", info, mdecl, mixin_info.methods[ mname ] }
          end
        end
      end
      jobs
    end

    private def build_classes : Array( Runtime::ObjectModel::RClass )
      @typed.types.compact_map do |name, info|
        next nil unless info.kind.class?
        slots        = info.reg_layout.try( &.total_size ) || 0
        finalize_idx = @func_index[ "#{name}#finalize" ]? || -1
        drop_idx     = @func_index[ "#{name}#__drop_fields" ]? || -1
        Runtime::ObjectModel::RClass.new( info.type_id, name, slots, finalize_idx, drop_idx, build_vtable( info ) )
      end
    end

    # Resolves each vtable slot to the chunk that actually implements it for
    # `info` : its own (or adopted-mixin) copy first, else the nearest
    # ancestor's — the fallback a subclass that neither overrides nor
    # re-includes the mixin relies on to still dispatch correctly.
    private def build_vtable( info : Frontend::TypeInfo ) : Array( Int32 )
      table = Array( Int32 ).new( info.vtable_size, -1 )
      info.vtable_layout.each do |method_name, idx|
        cur = info
        loop do
          if chunk_idx = @func_index[ "#{cur.name}##{method_name}" ]?
            table[ idx ] = chunk_idx
            break
          end
          sup = cur.superclass.try { |s| @typed.types[ s ]? }
          break unless sup
          cur = sup
        end
      end
      table
    end

    # Reconstructs the `NominalType` view of a `TypeInfo` : `TypedProgram`
    # only carries the resolved `TypeInfo`, not the `NominalType` instances
    # from Phase B's collection pass, so the compiler rebuilds an equivalent
    # one wherever `FunctionEmiter` needs a `Frontend::Type` (slot counting,
    # `self` binding).
    private def nominal_for( info : Frontend::TypeInfo ) : Frontend::NominalType
      kind = info.kind.struct? ? Frontend::TypeKind::Struct : Frontend::TypeKind::Object
      Frontend::NominalType.new( info.name, kind, info.type_id, info.layout, info.reg_layout )
    end

    private def compile_function( fn : Frontend::FuncDecl ) : IR::Chunk
      sig     = @typed.signatures[ fn.name ]
      emitter = FunctionEmiter.new( fn.name, fn.params.size, @func_index, @typed.signatures, @natives, @typed.types, nil, @global_index )
      fn.params.each_with_index { |p, i| emitter.bind_param( p.name, sig.params[ i ]? || Frontend::Type::UNKNOWN ) }
      emitter.enter_scope
      result = emitter.compile_body( fn.body )
      emitter.exit_scope
      emitter.emit_ret( result, emitter.slot_count( sig.ret ) )
      emitter.finish
    end

    # A method chunk's calling convention is `self` (1 slot for a class
    # reference, N slots for a struct value) followed by the declared
    # parameters. Style-1 ivar params (`def initialize( @x : T )`) have no
    # corresponding statement in the AST body : the implicit store is emitted
    # here, before the user-written body runs.
    private def compile_method( mangled : String, owner : Frontend::TypeInfo, decl : Frontend::FuncDecl, sig : Frontend::FuncSig ) : IR::Chunk
      # A module method is static : it takes no `self`, so its params (if any)
      # occupy the first registers, exactly like a free function.
      is_static = owner.kind.module?
      arity     = decl.params.size + ( is_static ? 0 : 1 )
      emitter   = FunctionEmiter.new( mangled, arity, @func_index, @typed.signatures, @natives, @typed.types, owner, @global_index )
      emitter.bind_param( "self", nominal_for( owner ) ) unless is_static
      decl.params.each_with_index { |p, i| emitter.bind_param( p.name, sig.params[ i ]? || Frontend::Type::UNKNOWN ) }
      decl.params.each { |p| emitter.store_ivar_param( owner, p.name ) if p.is_ivar }

      emitter.enter_scope
      result = emitter.compile_body( decl.body )
      emitter.exit_scope

      # A struct has no heap indirection: `initialize` runs in its own callee
      # frame, so the only way the constructed value reaches the call site is
      # by returning `self` itself through `RET` (see `compile_struct_new`).
      if owner.kind.struct? && decl.name == "initialize"
        emitter.emit_ret( emitter.self_register, emitter.slot_count( nominal_for( owner ) ) )
      else
        emitter.emit_ret( result, emitter.slot_count( sig.ret ) )
      end
      emitter.finish
    end

    # Auto-generated deep-drop: DROPs every reference field so a class's RAII
    # teardown recurses into fields it owns (architecture #4.2/§3.A).
    private def compile_drop_fields( info : Frontend::TypeInfo ) : IR::Chunk
      mangled = "#{info.name}#__drop_fields"
      emitter = FunctionEmiter.new( mangled, 1, @func_index, @typed.signatures, @natives, @typed.types, info, @global_index )
      emitter.bind_param( "self", nominal_for( info ) )
      info.layout.not_nil!.fields.select { |f| droppable?( f ) }.each { |f| emitter.emit_drop_field( info, f ) }
      emitter.emit_ret_nil
      emitter.finish
    end

    # `FieldSlot#is_ref` marks every reference-kind field (architecture #4.2's
    # `is_ref` bit) : but `String`/`Regex`/`Func` are plain Crystal-GC'd
    # values inside `Value`, not `HeapObject`s the VM's RAII protocol can
    # `DROP`. Only a reference to a user-defined *class* needs deep-drop.
    private def droppable?( f : Frontend::FieldSlot ) : Bool
      f.is_ref && f.type.is_a?( Frontend::NominalType ) && f.type.as( Frontend::NominalType ).kind.object?
    end

    private def compile_main : IR::Chunk
      emitter = FunctionEmiter.new( "main", 0, @func_index, @typed.signatures, @natives, @typed.types, nil, @global_index )
      emitter.enter_scope
      # Initialize module `@@vars` to their declared defaults before any
      # top-level statement (or module method) can observe them.
      @global_inits.each do |slot, default, _owner|
        next unless default
        vreg = emitter.compile_expr( default )
        emitter.emit_global_init( slot, vreg )
      end
      result = emitter.compile_body( @typed.top_level )
      emitter.exit_scope
      emitter.emit_ret( result )
      emitter.finish
    end
  end


end
