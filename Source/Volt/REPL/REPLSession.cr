module Volt::REPL


  class REPLSession
    getter history : Array(String)

    def initialize
      @history = [] of String
    end

    def evaluate( input : String, stdout : IO = STDOUT, stderr : IO = STDERR ) : REPLEvaluationResult
      full_program_source = @history.empty? ? input : @history.join( "\n" ) + "\n" + input

      begin
        typed = analyse!( full_program_source )
        unit = compile!( typed )
        value = execute!( unit, stdout, stderr )
        saved = declaration?( input )

        @history << input if saved

        REPLEvaluationResult.new(value, nil, saved)

      rescue ex : Frontend::CompilationError
        REPLEvaluationResult.new(nil, ex.bag, false)
      end
    end

    def clear : Nil
      @history.clear
    end

    #------------------------------------------------------------------------------------

    private def declaration?( input : String ) : Bool
      program = Frontend.parse( input, "<repl-temp>" )
      program.nodes.any? { |node| node.is_a?( Frontend::ADecl ) || node.is_a?( Frontend::Assign ) }
    rescue
      false
    end

    private def analyse!( source : String ) : Frontend::TypedProgram
      program = Frontend.parse( source, "<repl>" )
      analyser = Frontend::Analyser.new( program )
      analyser.analyse
    end

    private def compile!( typed : Frontend::TypedProgram ) : Compiler::Unit
      unit = Compiler::BytecodeCompiler.new( typed ).compile
      unit = Compiler::ConstFold.run( unit )
      unit = Compiler::Peephole.run( unit )
      unit
    end

    private def execute!( unit : Compiler::Unit, stdout : IO, stderr : IO ) : IR::Value
      vm = VM::Vm.new( unit, stdout, stderr )
      main_chunk = unit.chunks[ unit.main_index ]
      vm.call_chunk( main_chunk, [] of IR::Value )
    end

    #------------------------------------------------------------------------------------

  end


end
