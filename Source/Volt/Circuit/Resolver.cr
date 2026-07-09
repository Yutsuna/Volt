module Volt::Circuit


  # Result of dependency resolution: a merged `Program` (dependencies first,
  # entrypoint last, `Span.file` keeping per-node provenance) plus the
  # `file => source` map the diagnostic renderer needs.
  record Resolution,
    program : Frontend::Program,
    sources : Hash( String, String )


  # Walks the `@[Link]` graph of a project starting from the manifest
  # entrypoint and produces a single analysable `Program`.
  #
  # Each linked module loads every `.vl` file of its mapped directory
  # (recursively, sorted for determinism). A module is loaded once no matter
  # how many files link it (diamonds collapse); link cycles are reported as
  # diagnostics instead of overflowing the stack.
  class Resolver

    # Everything read so far, kept even when resolution fails so callers can
    # render diagnostics for partially loaded projects.
    getter sources : Hash( String, String )

    def initialize( @manifest : Manifest )
      @sources  = {} of String => String
      @bag      = Frontend::DiagnosticBag.new
      @visited  = Set( String ).new
      @stack    = [] of String
      @nodes    = [] of Frontend::ANode
    end


    def resolve : Resolution
      entry = parse_file( @manifest.entrypoint_path )
      visit_links( entry )
      append_nodes( entry )

      raise Frontend::CompilationError.new( @bag ) if @bag.errors?

      program = Frontend::Program.new( @nodes, @manifest.entrypoint_path, entry.loc )
      Resolution.new( program, @sources )
    end


    #------------------------------------------------------------------------------------


    # Loads `name` and (recursively, depth-first) everything it links, then
    # appends its nodes — post-order, so dependencies always come first.
    private def visit_module( name : String, link : Frontend::LinkDecl ) : Nil
      return if @visited.includes?( name )

      if @stack.includes?( name )
        chain = @stack[ @stack.index!( name ).. ] << name
        @bag << Frontend::Catalog::Circuit.circular_dependency( chain, link.loc )
        return
      end

      unless dir = @manifest.module_path( name )
        @bag << Frontend::Catalog::Circuit.link_unknown_module( name, link.loc, @manifest.modules.keys )
        return
      end

      @stack.push( name )
      programs = module_files( dir ).map { |file| parse_file( file ) }
      programs.each { |program| visit_links( program ) }
      @stack.pop

      @visited << name
      programs.each { |program| append_nodes( program ) }
    end

    private def visit_links( program : Frontend::Program ) : Nil
      program.nodes.each do |node|
        visit_module( node.module_name, node ) if node.is_a?( Frontend::LinkDecl )
      end
    end

    private def module_files( dir : String ) : Array( String )
      Dir.glob( File.join( dir, "**", "*.vl" ) ).sort!
    end

    private def parse_file( path : String ) : Frontend::Program
      source = File.read( path )
      @sources[ path ] = source
      Frontend.parse( source, path )
    end

    # `LinkDecl` nodes are resolver directives, not program content: they are
    # consumed here and must not reach the semantic pass.
    private def append_nodes( program : Frontend::Program ) : Nil
      program.nodes.each do |node|
        @nodes << node unless node.is_a?( Frontend::LinkDecl )
      end
    end

  end


end
