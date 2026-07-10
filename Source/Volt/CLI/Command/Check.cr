require "./ACommand"

module Volt::CLI


  class CheckCommand < ACommand
    register "check", "Static semantic code analysis"

    property input : String?
    property type : String = "syntax"
    property rules : String?
    property output : String = "json"
    property warn_as_error : Bool = false
    property metrics : Bool = false
    property unused : Bool = false

    #------------------------------------------------------------------------------------

    def execute( args : Array(String) )
      parse args

      input = @input || ( Dir.current if File.file?( File.join( Dir.current, Volt::Circuit::MANIFEST_NAME ) ) )
      fatal! "Missing source analyzer validation inputs (-i)" unless input

      if manifest_path = Volt::Circuit.discover( input )
        check_project( manifest_path )
      else
        check_file( input )
      end
    end

    #------------------------------------------------------------------------------------

    # Mono-file mode: no `Project.vl` above the input (original behaviour).
    private def check_file( file : String )
      source = begin
        File.read(file)
      rescue e
        fatal! "Cannot read '#{file}': #{e.message}"
      end

      Logger.info( "Running semantic analysis", "check" )

      typed = begin
        Frontend.analyse( Volt::Core.inject( Frontend.parse( source, file ) ) )
      rescue e : Frontend::CompilationError
        DiagnosticRenderer.new( Volt::Core.merge_sources( { file => source } ) ).render( e.bag )
        raise SystemExit.new
      end

      report typed
    end

    # Circuit mode: validate the manifest, resolve the whole `@[Link]` graph
    # from its entrypoint, and analyse the merged program (no execution).
    private def check_project( manifest_path : String )
      Logger.info( "Circuit manifest found: #{manifest_path}", "check" )

      manifest = begin
        Volt::Circuit.load( manifest_path )
      rescue e : Frontend::CompilationError
        DiagnosticRenderer.new( { manifest_path => File.read( manifest_path ) } ).render( e.bag )
        raise SystemExit.new
      end

      Logger.info( "Running semantic analysis on circuit `#{manifest.name}`", "check" )

      resolver = Volt::Circuit::Resolver.new( manifest )
      typed = begin
        Frontend.analyse( Volt::Core.inject( resolver.resolve.program ) )
      rescue e : Frontend::CompilationError
        DiagnosticRenderer.new( Volt::Core.merge_sources( resolver.sources ) ).render( e.bag )
        raise SystemExit.new
      end

      report typed
    end

    private def report( typed : Frontend::TypedProgram )
      fn_count = typed.functions.size
      Logger.info( "OK : #{fn_count} function(s), #{typed.top_level.size} top-level statement(s) type-checked", "check" )
    end

    #------------------------------------------------------------------------------------

    private def parse( args : Array(String))
      parser = OptionParser.parse( args ) do |p|
        p.banner = "Usage: volt check [options] [input_file_or_dir]"
        p.on("-i INPUT", "--input INPUT", "Code target directory or source file") { |v| @input = v }
        p.on("--type TYPE", "Type verification scope (syntax|semantic|style)") { |v| @type = v }
        p.on("--rules RULES", "Location path defining validation rule models") { |v| @rules = v }
        p.on("--output OUT", "Output validation layout formats") { |v| @output = v }
        p.on("--warn-as-error", "Style warning events will promote to runtime errors") { @warn_as_error = true }
        p.on("--metrics", "Gather code statistics and size metric benchmarks") { @metrics = true }
        p.on("--unused", "Flag unreferenced syntax structures and bindings") { @unused = true }
        p.on("-h", "--help", "Show help") { puts p; next }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end

      @input = args.first? if @input.nil?
    end

  end


end
