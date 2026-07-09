module Volt::Circuit

  extend self

  MANIFEST_NAME = "Project.vl"


  # Loads and validates a circuit manifest, returning a typed `Manifest`.
  #
  # `path` is either the `Project.vl` file itself or the project directory
  # containing it. Raises `Frontend::CompilationError` with `file:line:col`
  # diagnostics on any parse or validation failure.
  def load( path : String ) : Manifest
    manifest_path = File.directory?( path ) ? File.join( path, MANIFEST_NAME ) : path

    unless File.file?( manifest_path )
      fail_with Frontend::Catalog::Circuit.manifest_not_found( manifest_path )
    end

    program = Frontend.parse( File.read( manifest_path ), manifest_path )
    decl    = single_circuit_decl( program, manifest_path )
    root    = File.dirname( File.expand_path( manifest_path ) )

    validate( decl, root )
  end


  private def single_circuit_decl( program : Frontend::Program, file : String ) : Frontend::CircuitDecl
    decls = program.nodes.select( Frontend::CircuitDecl )

    case decls.size
    when 0 then fail_with Frontend::Catalog::Circuit.no_circuit_block( file )
    when 1 then decls.first
    else        fail_with Frontend::Catalog::Circuit.multiple_circuit_blocks( decls[ 1 ].loc, decls[ 0 ].loc )
    end
  end


  private def validate( decl : Frontend::CircuitDecl, root : String ) : Manifest
    bag = Frontend::DiagnosticBag.new

    runtime    = decl.runtime
    entrypoint = decl.entrypoint
    bag << Frontend::Catalog::Circuit.missing_entry( "runtime", decl.loc )    unless runtime
    bag << Frontend::Catalog::Circuit.missing_entry( "entrypoint", decl.loc ) unless entrypoint

    if entrypoint
      if full = confined_path( root, entrypoint )
        bag << Frontend::Catalog::Circuit.entrypoint_not_found( entrypoint, decl.loc ) unless File.file?( full )
      else
        bag << Frontend::Catalog::Circuit.path_escapes_project( entrypoint, decl.loc )
      end
    end

    modules = validate_modules( decl, root, bag )

    raise Frontend::CompilationError.new( bag ) if bag.errors?
    Manifest.new( decl.name, runtime.not_nil!, entrypoint.not_nil!, modules, root )
  end


  private def validate_modules( decl : Frontend::CircuitDecl, root : String,
                                bag : Frontend::DiagnosticBag ) : Hash( String, String )
    modules = {} of String => String
    seen    = {} of String => Frontend::Span

    decl.modules.try( &.pairs.each do |( key_expr, value_expr )|
      # ParseCircuit guarantees plain (non-interpolated) string literals.
      key   = key_expr.as( Frontend::StringLit )
      value = value_expr.as( Frontend::StringLit )

      if first = seen[ key.value ]?
        bag << Frontend::Catalog::Circuit.duplicate_module( key.value, key.loc, first )
        next
      end
      seen[ key.value ] = key.loc

      if full = confined_path( root, value.value )
        if File.directory?( full )
          modules[ key.value ] = value.value
        else
          bag << Frontend::Catalog::Circuit.module_dir_not_found( key.value, value.value, value.loc )
        end
      else
        bag << Frontend::Catalog::Circuit.path_escapes_project( value.value, value.loc )
      end
    end )

    modules
  end


  # Resolves `rel` against `root` and returns the absolute path, or `nil`
  # when the path is absolute or climbs out of the project root via `../`.
  private def confined_path( root : String, rel : String ) : String?
    return nil if rel.starts_with?( '/' )

    full = File.expand_path( rel, root )
    return nil unless full == root || full.starts_with?( root + File::SEPARATOR )
    full
  end


  private def fail_with( diagnostic : Frontend::Diagnostic ) : NoReturn
    bag = Frontend::DiagnosticBag.new
    bag << diagnostic
    raise Frontend::CompilationError.new( bag )
  end


end
