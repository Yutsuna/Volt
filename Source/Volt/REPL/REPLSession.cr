module Volt::REPL


  class REPLSession
    getter history : Array(String)
    getter state : IncrementalState
    getter vm : VM::Vm?

    def initialize
      @history = [] of String
      @state = IncrementalState.new
      @vm = nil
    end

    def evaluate( input : String, stdout : IO = STDOUT, stderr : IO = STDERR ) : REPLEvaluationResult
      compile_and_run( input, "<repl>", stdout, stderr, is_evaluate: true )
    end

    def load_source( source : String, path : String, stdout : IO = STDOUT, stderr : IO = STDERR ) : REPLEvaluationResult
      compile_and_run( source, path, stdout, stderr, is_evaluate: false )
    end

    def clear : Nil
      @history.clear
      @state.clear
      @vm = nil
    end

    private def compile_and_run( input : String, filename : String, stdout : IO, stderr : IO, is_evaluate : Bool ) : REPLEvaluationResult
      begin
        program = Frontend.parse( input, filename )
        analyser = Frontend::IncrementalAnalyser.new( program, @state )
        typed = analyser.analyse

        # Compile delta
        compiler = REPLDeltaCompiler.new(
          typed,
          @state.func_index,
          @state.global_index,
          @state.natives,
          @state.chunk_count,
          @state.top_level_globals,
          @state.globals_count
        )
        delta_unit = compiler.compile

        # Create or extend VM
        vm = @vm
        if vm
          vm.extend( delta_unit )
        else
          vm = VM::Vm.new( delta_unit, stdout, stderr )
          @vm = vm
        end

        # Calculate chunk execution index before we update the count
        index = @state.chunk_count + delta_unit.main_index

        # Merge compiler results into session state BEFORE execution
        # to guarantee synchronization even if execution raises a runtime exception.
        @state.func_index.merge!( compiler.func_index )
        @state.global_index.merge!( compiler.global_index )
        @state.globals_count = delta_unit.num_globals
        @state.natives.clear
        compiler.natives.natives.each do |nf|
          @state.natives << nf
        end
        @state.chunk_count += delta_unit.chunks.size

        # Execute
        values = vm.call_chunk_at( index )

        saved = false
        if is_evaluate
          # Avoid double-parsing by inspecting already parsed nodes
          saved = program.nodes.any? { |node| node.is_a?( Frontend::ADecl ) || node.is_a?( Frontend::Assign ) }
          @history << input if saved
        end

        REPLEvaluationResult.new( values.first?, nil, saved )

      rescue ex : VM::VoltRuntimeError
        stderr.puts( "runtime error: #{ex.message || "unknown"}" )
        REPLEvaluationResult.new( nil, nil, false )
      rescue ex : DivisionByZeroError
        stderr.puts( "runtime error: division by zero" )
        REPLEvaluationResult.new( nil, nil, false )
      rescue ex : Frontend::CompilationError
        REPLEvaluationResult.new( nil, ex.bag, false )
      end
    end
  end


end
