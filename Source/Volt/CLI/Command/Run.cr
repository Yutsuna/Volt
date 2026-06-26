require "./ACommand"


module Volt::CLI


  class RunCommand < ACommand
    register "run", "Interpret the file input"

    property input : String?
    property program_args = [] of String
    property from_stdin : Bool = false

    def execute(args : Array(String))
      parse args
      if @from_stdin
        from_stdin
      else
        from_source
      end
    end

    private def parse( args : Array(String) )
      if args_index = args.index("--")
        @program_args = args[(args_index + 1)..-1]
        args = args[0...args_index]
      end

      parser = OptionParser.parse(args) do |p|
        p.banner = "Usage: volt run [options] [input_file] [-- ...]"
        p.on("-i INPUT", "--input INPUT", "File input source program") { |v| @input = v }
        p.on("-s", "--stdin", "Read input from stdin") { @from_stdin = true }
        p.on("-h", "--help", "Show help") { puts p; next }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end

      @input = args.first? if @input.nil?
      fatal! "Cannot specify both input file and stdin" if @input && @from_stdin
      fatal! "Missing input file parameter (-i)" unless @input || @from_stdin
    end

    private def from_stdin
      source = begin
        STDIN.gets_to_end
      rescue e
        fatal! "Cannot read from stdin: #{e.message}"
      end
      filename = "<stdin>"
      interpret( source, filename )
    end

    private def from_source
      file = @input.not_nil!
      source = begin
        File.read file
      rescue e
        fatal! "Cannot read '#{file}': #{e.message}"
      end
      filename = file
      interpret( source, filename )
    end

    private def interpret(source : String, filename : String)
      typed = Frontend.analyse( source, filename )

      unit = Compiler::BytecodeCompiler.new( typed ).compile
      unit = Compiler::ConstFold.run( unit )
      unit = Compiler::EscapeAnalysis.run( unit )
      unit = Compiler::Peephole.run( unit )

      code = VM::Vm.new( unit ).run
    rescue e : Frontend::CompilationError
      DiagnosticRenderer.new( { filename => source } ).render( e.bag )
      raise SystemExit.new
    end

  end


end
