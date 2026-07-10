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
      elsif file = @input
        from_input( file )
      elsif manifest_path = Volt::Circuit.discover( Dir.current )
        run_project( manifest_path )
      else
        fatal! "Missing input file parameter (-i)"
      end
    end

    #------------------------------------------------------------------------------------

    private def parse( args : Array(String) )
      if args_index = args.index("--")
        @program_args = args[(args_index + 1)..-1]
        args = args[0...args_index]
      end

      parser = OptionParser.parse(args) do |p|
        p.banner = "Usage: volt run [options] [input_file] [-- ...]"
        p.on("-i INPUT", "--input INPUT", "File input source program") { |v| @input = v }
        p.on("-s", "--stdin", "Read input from stdin") { @from_stdin = true }
        p.on("-h", "--help", "Show help") { puts p; raise RequestExit.new }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end

      @input = args.first? if @input.nil?
      fatal! "Cannot specify both input file and stdin" if @input && @from_stdin
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

    # `file` may belong to a circuit project (a `Project.vl` found walking up
    # from it) : in that case the project's entrypoint is run, not `file`
    # itself. Otherwise falls back to the original mono-file behaviour.
    private def from_input( file : String )
      if manifest_path = Volt::Circuit.discover( file )
        run_project( manifest_path )
      else
        run_file( file )
      end
    end

    private def run_file( file : String )
      source = begin
        File.read file
      rescue e
        fatal! "Cannot read '#{file}': #{e.message}"
      end
      filename = File.expand_path( file )
      interpret( source, filename )
    end

    # Circuit mode: resolve the whole `@[Link]` graph from the manifest
    # entrypoint into one merged `Program`, then compile/run it as usual.
    # `DiagnosticRenderer` gets the full multi-file source map so errors
    # from any linked module still point at `file:line:col`.
    private def run_project( manifest_path : String )
      manifest = begin
        Volt::Circuit.load( manifest_path )
      rescue e : Frontend::CompilationError
        DiagnosticRenderer.new( { manifest_path => File.read( manifest_path ) } ).render( e.bag )
        raise SystemExit.new
      end

      resolver = Volt::Circuit::Resolver.new( manifest )
      resolution = begin
        resolver.resolve
      rescue e : Frontend::CompilationError
        DiagnosticRenderer.new( resolver.sources ).render( e.bag )
        raise SystemExit.new
      end

      typed = begin
        Frontend.analyse( Volt::Core.inject( resolution.program ) )
      rescue e : Frontend::CompilationError
        DiagnosticRenderer.new( Volt::Core.merge_sources( resolution.sources ) ).render( e.bag )
        raise SystemExit.new
      end

      execute_vm( typed )
    end

    private def interpret( source : String, filename : String )
      typed = Frontend.analyse( Volt::Core.inject( Frontend.parse( source, filename ) ) )
      execute_vm( typed )
    rescue e : Frontend::CompilationError
      DiagnosticRenderer.new( Volt::Core.merge_sources( { filename => source } ) ).render( e.bag )
      raise SystemExit.new
    end

    private def execute_vm( typed : Frontend::TypedProgram )
      unit = Compiler::BytecodeCompiler.new( typed ).compile
      unit = Compiler::ConstFold.run( unit )
      unit = Compiler::EscapeAnalysis.run( unit )
      unit = Compiler::Peephole.run( unit )

      VM::Vm.new( unit, STDOUT, STDERR ).run
    end

  end


end
