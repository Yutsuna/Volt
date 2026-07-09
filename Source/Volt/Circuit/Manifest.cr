module Volt::Circuit


  # Typed, validated view of a `Project.vl` circuit manifest.
  #
  # `modules` maps module names to their directory, both kept relative to
  # `root` (the absolute directory containing the manifest) — the resolver
  # joins them when loading files.
  struct Manifest
    getter name       : String
    getter runtime    : String
    getter entrypoint : String
    getter modules    : Hash( String, String )
    getter root       : String

    def initialize( @name : String, @runtime : String, @entrypoint : String,
                    @modules : Hash( String, String ), @root : String )
    end

    def entrypoint_path : String
      File.join( @root, @entrypoint )
    end

    def module_path( name : String ) : String?
      @modules[ name ]?.try { |dir| File.join( @root, dir ) }
    end
  end


end
