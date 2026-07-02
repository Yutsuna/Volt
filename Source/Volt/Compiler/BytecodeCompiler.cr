require "./Unit"
require "./NativeTable"
require "./FunctionEmiter"

module Volt::Compiler


  class BytecodeCompiler
    @func_index : Hash( String, Int32 )

    #------------------------------------------------------------------------------------

    def initialize( @typed : Frontend::TypedProgram )
      @func_index = {} of String => Int32
      @natives    = NativeTable.new
    end

    #------------------------------------------------------------------------------------

    def compile : Unit
      @typed.functions.each_with_index { |fn, i| @func_index[ fn.name ] = i }

      method_jobs = collect_method_jobs
      next_idx    = @typed.functions.size
      method_jobs.each do |mangled, _owner, _decl|
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
      chunks.concat( method_jobs.map { |mangled, owner, decl| compile_method( mangled, owner, decl ) } )
      chunks.concat( drop_fields_types.map { |info| compile_drop_fields( info ) } )
      chunks << compile_main

      Unit.new( chunks, chunks.size - 1, @natives.natives, build_classes )
    end

    #------------------------------------------------------------------------------------

    private def collect_method_jobs : Array( { String, Frontend::TypeInfo, Frontend::FuncDecl } )
      jobs = [] of { String, Frontend::TypeInfo, Frontend::FuncDecl }
      @typed.types.each_value do |info|
        if info.kind.struct?
          # A struct has no subclassing/mixins, so every method lowers
          # straight to a direct call (Phase 3) — no vtable dependency, so
          # every method (not just initialize) is safe to compile eagerly.
          info.methods_ast.each_value do |decl|
            next if decl.is_abstract
            jobs << { "#{info.name}##{decl.name}", info, decl }
          end
        elsif info.kind.class?
          # Class method dispatch (vtables, mixin ITables, implicit-self
          # calls) is Phase 4 : compiling other method bodies eagerly here
          # would surface their unlowered call sites as a crash instead of
          # the clean "Phase 4" boundary error raised at the actual call
          # site. `initialize`/`finalize` are exempt — RAII (Phase 2) needs
          # them regardless of Phase 4.
          if init = info.methods_ast[ "initialize" ]?
            jobs << { "#{info.name}#initialize", info, init }
          end
          if fin = info.methods_ast[ "finalize" ]?
            jobs << { "#{info.name}#finalize", info, fin }
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
        Runtime::ObjectModel::RClass.new( info.type_id, name, slots, finalize_idx, drop_idx )
      end
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
      emitter = FunctionEmiter.new( fn.name, fn.params.size, @func_index, @typed.signatures, @natives, @typed.types )
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
    private def compile_method( mangled : String, owner : Frontend::TypeInfo, decl : Frontend::FuncDecl ) : IR::Chunk
      sig     = owner.methods[ decl.name ]
      emitter = FunctionEmiter.new( mangled, decl.params.size + 1, @func_index, @typed.signatures, @natives, @typed.types, owner )
      emitter.bind_param( "self", nominal_for( owner ) )
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
      emitter = FunctionEmiter.new( mangled, 1, @func_index, @typed.signatures, @natives, @typed.types, info )
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
      emitter = FunctionEmiter.new( "main", 0, @func_index, @typed.signatures, @natives, @typed.types )
      emitter.enter_scope
      result = emitter.compile_body( @typed.top_level )
      emitter.exit_scope
      emitter.emit_ret( result )
      emitter.finish
    end
  end


end
