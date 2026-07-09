require "./ACommand"


module Volt::CLI


  class CircuitCommand < ACommand
    register "circuit", "Create or update the Project.vl file"

    property project_directory : String?

    def execute(args : Array(String))
      parse args

      dir = File.expand_path( @project_directory || "." )
      fatal! "Directory not found: #{dir}" unless Dir.exists?( dir )

      Logger.info( "Configuring project metadata in: #{dir}", "circuit" )
      Logger.progress( "Scanning workspace directories...", "1/2" )

      manifest_path = File.join( dir, Volt::Circuit::MANIFEST_NAME )

      content = if File.file?( manifest_path )
                  update_manifest( dir, manifest_path )
                else
                  create_manifest( dir )
                end

      before = File.file?( manifest_path ) ? File.read( manifest_path ) : nil
      File.write( manifest_path, content )

      status = before == content ? "already up to date" : "synchronized successfully"
      Logger.progress( "#{Volt::Circuit::MANIFEST_NAME} #{status}", "2/2", finished: true )
    rescue e : Frontend::CompilationError
      fatal! "Failed to load existing #{Volt::Circuit::MANIFEST_NAME}: #{e.message}"
    end

    private def parse(args : Array(String))
      parser = OptionParser.parse( args ) do |p|
        p.banner = "Usage: volt circuit [options]"
        p.on("-d DIR", "--dir DIR", "Project directory path") { |v| @project_directory = v }
        p.on("-h", "--help", "Show help") { puts p; next }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end
      @project_directory = args.first? if @project_directory.nil?
    end

    # Fresh `Project.vl`: scans `Source/` for module directories and the
    # conventional `Source/Main.vl` entrypoint.
    private def create_manifest(root : String) : String
      entrypoint = "Source/Main.vl"
      fatal! "No entrypoint found: expected #{File.join(root, entrypoint)}" \
        unless File.file?( File.join( root, entrypoint ) )

      name    = File.basename( root )
      modules = scan_modules( root )

      format_manifest( name, Volt::VERSION, entrypoint, modules )
    end

    # Existing `Project.vl`: preserves name/runtime/entrypoint and any custom
    # module mapping, adds newly discovered module directories, and flags
    # (without deleting) modules/entrypoint that have disappeared from disk.
    #
    # Reads the manifest structurally instead of `Circuit.load` (which is
    # strict about on-disk existence) : synchronizing a manifest whose
    # module directory was deleted is exactly the case this command exists
    # to surface and fix.
    private def update_manifest(root : String, manifest_path : String) : String
      name, runtime, entrypoint, modules = read_manifest( manifest_path )

      unless File.file?( File.join( root, entrypoint ) )
        Logger.warn( "Entrypoint points to a missing file: #{entrypoint}", "circuit" )
      end

      on_disk = scan_modules( root )
      merged  = modules.dup
      on_disk.each do |mod_name, rel|
        next if merged.has_key?( mod_name )
        Logger.info( "New module discovered: \"#{mod_name}\" => \"#{rel}\"", "circuit" )
        merged[ mod_name ] = rel
      end

      modules.each do |mod_name, rel|
        unless Dir.exists?( File.join( root, rel ) )
          Logger.warn( "Module \"#{mod_name}\" points to a missing directory: #{rel}", "circuit" )
        end
      end

      format_manifest( name, runtime, entrypoint, merged )
    end

    # Structural read of a `Project.vl`: single circuit block required, but
    # unlike `Circuit.load` this does not check module/entrypoint paths
    # against disk, since fixing exactly that is this command's purpose.
    private def read_manifest(manifest_path : String) : {String, String, String, Hash(String, String)}
      program = Frontend.parse( File.read( manifest_path ), manifest_path )
      decls   = program.nodes.select( Frontend::CircuitDecl )

      case decls.size
      when 0 then fatal! "No `circuit` block found in #{manifest_path}"
      when 1 then decl = decls.first
      else        fatal! "Multiple `circuit` blocks found in #{manifest_path}"
      end

      runtime    = decl.runtime
      entrypoint = decl.entrypoint
      fatal! "Missing `runtime` entry in #{manifest_path}"    unless runtime
      fatal! "Missing `entrypoint` entry in #{manifest_path}" unless entrypoint

      modules = {} of String => String
      decl.modules.try( &.pairs.each do |( key_expr, value_expr )|
        key   = key_expr.as?( Frontend::StringLit )
        value = value_expr.as?( Frontend::StringLit )
        next unless key && value
        modules[ key.value ] = value.value
      end )

      { decl.name, runtime, entrypoint, modules }
    end

    # First-level directories under `Source/` containing at least one `.vl`
    # file (recursively), keyed by directory basename.
    private def scan_modules(root : String) : Hash(String, String)
      modules    = {} of String => String
      source_dir = File.join( root, "Source" )
      return modules unless Dir.exists?( source_dir )

      Dir.children( source_dir ).sort.each do |entry|
        full = File.join( source_dir, entry )
        next unless Dir.exists?( full )
        next if Dir.glob( File.join( full, "**", "*.vl" ) ).empty?

        modules[ entry ] = Path.new( full ).relative_to( root ).to_s
      end

      modules
    end

    private def format_manifest(name : String, runtime : String, entrypoint : String,
                                modules : Hash(String, String)) : String
      io = String::Builder.new
      io << "circuit " << quote( name ) << "\n"
      io << "{\n"
      io << "  runtime " << quote( runtime ) << "\n"
      io << "  entrypoint " << quote( entrypoint ) << "\n"

      unless modules.empty?
        width = modules.keys.max_of { |k| quote( k ).size }
        io << "\n"
        io << "  modules(\n"
        modules.each do |k, v|
          io << "    " << quote( k ).ljust( width ) << " => " << quote( v ) << ",\n"
        end
        io << "  )\n"
      end

      io << "}\n"
      io.to_s
    end

    private def quote(s : String) : String
      %("#{s.gsub('\\', "\\\\\\\\").gsub('"', "\\\"")}")
    end

  end


end
