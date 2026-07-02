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
      chunks = @typed.functions.map { |fn| compile_function( fn ) }
      chunks << compile_main
      Unit.new( chunks, chunks.size - 1, @natives.natives )
    end

    #------------------------------------------------------------------------------------

    private def compile_function( fn : Frontend::FuncDecl ) : IR::Chunk
      emitter = FunctionEmiter.new( fn.name, fn.params.size, @func_index, @typed.signatures, @natives )
      fn.params.each { |p| emitter.bind_param( p.name ) }
      result = emitter.compile_body( fn.body )
      emitter.emit_ret( result )
      emitter.finish
    end

    private def compile_main : IR::Chunk
      emitter = FunctionEmiter.new( "main", 0, @func_index, @typed.signatures, @natives )
      result = emitter.compile_body( @typed.top_level )
      emitter.emit_ret( result )
      emitter.finish
    end
  end


end
