require "./Embedded"


module Volt::Core

  extend self

  # Pseudo-path prefix for embedded sources : diagnostics render it as-is,
  # making Core provenance obvious (`<lib>/IO.vl:3:5`).
  EMBEDDED_PREFIX = "<lib>/"

  @@sources : Hash( String, String )?
  @@program : Frontend::Program?


  # `file => source` map of the Core stdlib, keyed the same way `Span.file`
  # is stamped on its nodes — merge into the `DiagnosticRenderer` source map
  # so an error inside the Core renders a proper `file:line:col` excerpt.
  #
  # `VOLT_CORE=<dir>` overrides the embedded sources with the textual `.vl`
  # files of that directory (sorted glob, like `Circuit::Resolver`), for
  # developing the stdlib without rebuilding the compiler.
  def sources : Hash( String, String )
    @@sources ||= load_sources
  end


  # The Core stdlib as one parsed `Program`, memoised per process : parsed
  # exactly once no matter how many times it is injected (REPL turns,
  # circuit + mono-file runs in specs, ...) — startup stays flat.
  #
  # Parse errors raise `Frontend::CompilationError`; callers already render
  # those, and `sources` is registered so the excerpt resolves.
  def program : Frontend::Program
    build_program
  end


  # Returns `user` with the Core nodes prepended — dependencies first, same
  # ordering contract as `Circuit::Resolver#append_nodes`. The resulting
  # program keeps the user's `file`/`loc` identity.
  def inject( user : Frontend::Program ) : Frontend::Program
    Frontend::Program.new( program.nodes + user.nodes, user.file, user.loc )
  end


  # Merges the Core sources into a user source map (for diagnostics).
  def merge_sources( user : Hash( String, String ) ) : Hash( String, String )
    sources.merge( user )
  end


  #------------------------------------------------------------------------------------


  private def load_sources : Hash( String, String )
    srcs = {} of String => String

    if dir = ENV[ "VOLT_CORE" ]?
      Dir.glob( File.join( dir, "**", "*.vl" ) ).sort!.each do |file|
        srcs[ File.expand_path( file ) ] = File.read( file )
      end
    else
      EMBEDDED_SOURCES.each do |( name, source )|
        srcs[ EMBEDDED_PREFIX + name ] = source
      end
    end

    # Top-level definitions coming from these files may be shadowed by user
    # code (`Analyser` consults this set) : a program redeclaring `puts` is
    # reopening the Core, not colliding with it.
    srcs.each_key { |file| Frontend.core_files << file }
    srcs
  end


  private def build_program : Frontend::Program
    nodes = [] of Frontend::ANode
    loc   = Frontend::Span.new( "<lib>", 1_u32, 1_u32, 0_u32 )

    sources.each do |file, source|
      parsed = Frontend.parse( source, file )
      # `LinkDecl` is a resolver directive — meaningless inside the Core.
      parsed.nodes.each { |node| nodes << node unless node.is_a?( Frontend::LinkDecl ) }
    end

    Frontend::Program.new( nodes, "<lib>", loc )
  end


end
