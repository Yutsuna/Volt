# Source/Volt/REPL/REPLDeltaCompiler.cr
require "../Compiler/Unit"
require "../Compiler/NativeTable"
require "../Compiler/FunctionEmiter"

module Volt::REPL

  class REPLDeltaCompiler
    getter func_index : Hash( String, Int32 )
    getter global_index : Hash( String, Int32 )
    getter natives : Compiler::NativeTable
    getter globals_count : Int32

    @typed : Frontend::TypedProgram
    @existing_chunk_count : Int32
    @global_inits : Array( { Int32, Frontend::AExpr?, String } )

    def initialize(
      @typed : Frontend::TypedProgram,
      existing_func_index : Hash( String, Int32 ),
      existing_global_index : Hash( String, Int32 ),
      existing_natives : Array( Compiler::NativeFunc ),
      @existing_chunk_count : Int32,
      top_level_globals : Hash( String, Frontend::Type ),
      existing_globals_count : Int32
    )
      @func_index = existing_func_index.dup
      @global_index = existing_global_index.dup
      @globals_count = existing_globals_count
      
      @natives = Compiler::NativeTable.new
      existing_natives.each do |nf|
        @natives.intern( nf.lib, nf.name )
      end
      
      @global_inits = [] of { Int32, Frontend::AExpr?, String }
      
      # Ensure all top-level globals from the incremental state have slots in global_index.
      top_level_globals.each do |name, type|
        if !@global_index.has_key?( name )
          @global_index[ name ] = @globals_count
          @globals_count += slot_count( type )
        end
      end
    end

    def compile : Compiler::Unit
      collect_globals

      # Register new functions
      @typed.functions.each_with_index do |fn, i|
        @func_index[ fn.name ] = @existing_chunk_count + i
      end

      method_jobs = collect_method_jobs
      next_idx    = @existing_chunk_count + @typed.functions.size
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

      # Collect classes that need a synthesized destructor chunk
      dtor_classes = @typed.types.values.select do |info|
        info.kind.class? && ( @func_index[ "#{info.name}#finalize" ]? || @func_index[ "#{info.name}#__drop_fields" ]? )
      end
      dtor_classes.each do |info|
        @func_index[ "#{info.name}#__dtor" ] = next_idx
        next_idx += 1
      end

      # Compile chunks in the exact same order they were registered.
      # Note: The compiled chunks will only be the new ones compiled in this delta.
      chunks = @typed.functions.map { |fn| compile_function( fn ) }
      chunks.concat( method_jobs.map { |mangled, owner, decl, sig| compile_method( mangled, owner, decl, sig ) } )
      chunks.concat( drop_fields_types.map { |info| compile_drop_fields( info ) } )

      # Compile synthesized destructor chunks
      dtor_chunks = dtor_classes.map do |info|
        finalize_idx = @func_index[ "#{info.name}#finalize" ]? || -1
        drop_idx     = @func_index[ "#{info.name}#__drop_fields" ]? || -1
        compile_dtor( info.name, finalize_idx, drop_idx )
      end
      chunks.concat( dtor_chunks )

      chunks << compile_main

      # The main_index of this delta unit will be the last chunk in our local chunks array,
      # which compiles the new top-level expressions/statements.
      main_index = chunks.size - 1

      # Build classes that are defined/referenced in this typed program.
      # Note: build_classes uses build_vtable which resolves method chunk indices using @func_index,
      # which now includes both existing and newly registered methods.
      Compiler::Unit.new( chunks, main_index, @natives.natives, build_classes, @globals_count )
    end

    private def slot_count( type : Frontend::Type ) : Int32
      if type.is_a?( Frontend::NominalType ) && type.kind.struct?
        type.reg_layout.try( &.total_size ) || 1
      else
        1
      end
    end

    #------------------------------------------------------------------------------------

    private def collect_globals : Nil
      @typed.program.nodes.each { |node| collect_module_globals( node ) if node.is_a?( Frontend::ModuleDecl ) }
    end

    private def collect_module_globals( mod : Frontend::ModuleDecl ) : Nil
      mod.body.each do |node|
        case node
        when Frontend::ClassVarDecl
          key = "#{mod.name}::#{node.name}"
          if !@global_index.has_key?( key )
            slot = @globals_count
            @global_index[ key ] = slot
            @globals_count += 1
            @global_inits << { slot, node.value, mod.name }
          end
        when Frontend::ModuleDecl
          collect_module_globals( node )
        end
      end
    end

    #------------------------------------------------------------------------------------

    private def collect_method_jobs : Array( { String, Frontend::TypeInfo, Frontend::FuncDecl, Frontend::FuncSig } )
      jobs = [] of { String, Frontend::TypeInfo, Frontend::FuncDecl, Frontend::FuncSig }
      @typed.types.each_value do |info|
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
        dtor_idx     = @func_index[ "#{name}#__dtor" ]? || -1
        Runtime::ObjectModel::RClass.new( info.type_id, name, slots, finalize_idx, drop_idx, dtor_idx, build_vtable( info ) )
      end
    end

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

    private def nominal_for( info : Frontend::TypeInfo ) : Frontend::NominalType
      kind = info.kind.struct? ? Frontend::TypeKind::Struct : Frontend::TypeKind::Object
      Frontend::NominalType.new( info.name, kind, info.type_id, info.layout, info.reg_layout )
    end

    private def compile_function( fn : Frontend::FuncDecl ) : IR::Chunk
      sig     = @typed.signatures[ fn.name ]
      emitter = Compiler::FunctionEmiter.new( fn.name, fn.params.size, @func_index, @typed.signatures, @natives, @typed.types, nil, @global_index )
      fn.params.each_with_index { |p, i| emitter.bind_param( p.name, sig.params[ i ]? || Frontend::Type::UNKNOWN ) }
      emitter.enter_scope
      result = emitter.compile_body( fn.body )
      emitter.exit_scope
      emitter.emit_ret( result, emitter.slot_count( sig.ret ) )
      emitter.finish
    end

    private def compile_method( mangled : String, owner : Frontend::TypeInfo, decl : Frontend::FuncDecl, sig : Frontend::FuncSig ) : IR::Chunk
      is_static = owner.kind.module?
      arity     = decl.params.size + ( is_static ? 0 : 1 )
      emitter   = Compiler::FunctionEmiter.new( mangled, arity, @func_index, @typed.signatures, @natives, @typed.types, owner, @global_index )
      emitter.bind_param( "self", nominal_for( owner ) ) unless is_static
      decl.params.each_with_index { |p, i| emitter.bind_param( p.name, sig.params[ i ]? || Frontend::Type::UNKNOWN ) }
      decl.params.each { |p| emitter.store_ivar_param( owner, p.name ) if p.is_ivar }

      emitter.enter_scope
      result = emitter.compile_body( decl.body )
      emitter.exit_scope

      if owner.kind.struct? && decl.name == "initialize"
        emitter.emit_ret( emitter.self_register, emitter.slot_count( nominal_for( owner ) ) )
      else
        emitter.emit_ret( result, emitter.slot_count( sig.ret ) )
      end
      emitter.finish
    end

    private def compile_drop_fields( info : Frontend::TypeInfo ) : IR::Chunk
      mangled = "#{info.name}#__drop_fields"
      emitter = Compiler::FunctionEmiter.new( mangled, 1, @func_index, @typed.signatures, @natives, @typed.types, info, @global_index )
      emitter.bind_param( "self", nominal_for( info ) )
      info.layout.not_nil!.fields.select { |f| droppable?( f ) }.each { |f| emitter.emit_drop_field( info, f ) }
      emitter.emit_ret_nil
      emitter.finish
    end

    private def compile_dtor( class_name : String, finalize_idx : Int32, drop_idx : Int32 ) : IR::Chunk
      code = [] of IR::Instruction
      
      if finalize_idx >= 0
        code << IR::Instruction.abc( IR::Opcode::MOVE, 2, 0, 0 )
        code << IR::Instruction.abc( IR::Opcode::CALL, 1, finalize_idx, 1 )
      end

      if drop_idx >= 0
        code << IR::Instruction.abc( IR::Opcode::MOVE, 2, 0, 0 )
        code << IR::Instruction.abc( IR::Opcode::CALL, 1, drop_idx, 1 )
      end

      code << IR::Instruction.abx( IR::Opcode::RET, 0, 0 )

      chunk = IR::Chunk.new( "#{class_name}#__dtor", 1 )
      chunk.num_registers = 3
      chunk.code = code
      chunk
    end

    private def droppable?( f : Frontend::FieldSlot ) : Bool
      f.is_ref && f.type.is_a?( Frontend::NominalType ) && f.type.as( Frontend::NominalType ).kind.object?
    end

    private def compile_main : IR::Chunk
      emitter = Compiler::FunctionEmiter.new( "main", 0, @func_index, @typed.signatures, @natives, @typed.types, nil, @global_index )
      emitter.enter_scope
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
