require "spec"
require "file_utils"
require "../../Source/Volt/__all__"


module Volt::Spec

  RESOLVER_FIXTURES = File.expand_path( File.join( __DIR__, "..", "..", "Samples", "Tests", "Circuits" ) )


  private def self.resolve_fixture( name : String ) : Volt::Circuit::Resolution
    manifest = Volt::Circuit.load( File.join( RESOLVER_FIXTURES, name ) )
    Volt::Circuit::Resolver.new( manifest ).resolve
  end

  private def self.module_names( program : Volt::Frontend::Program ) : Array( String )
    program.nodes.select( Volt::Frontend::ModuleDecl ).map( &.name )
  end


  describe "Volt::Circuit::Resolver" do

    it "resolves TwoDeps in topological order, entrypoint last" do
      resolution = resolve_fixture( "TwoDeps" )
      program    = resolution.program

      # Dependencies come before the entrypoint nodes; Main.vl links Models
      # first, and Components (which itself links Models) after.
      module_names( program ).should eq( [ "Models", "Components" ] )

      root = File.join( RESOLVER_FIXTURES, "TwoDeps" )
      program.file.should eq( File.join( root, "Source", "Main.vl" ) )
      program.nodes.last.loc.file.should eq( File.join( root, "Source", "Main.vl" ) )
      program.nodes.first.loc.file.should eq( File.join( root, "Source", "Models", "User.vl" ) )

      resolution.sources.keys.sort!.should eq( [
        File.join( root, "Source", "Components", "Logger.vl" ),
        File.join( root, "Source", "Main.vl" ),
        File.join( root, "Source", "Models", "User.vl" ),
      ] )
    end

    it "strips LinkDecl nodes from the merged program" do
      program = resolve_fixture( "TwoDeps" ).program
      program.nodes.any?( Volt::Frontend::LinkDecl ).should be_false
    end

    it "resolves the DiamandDeps diamond, loading Core exactly once" do
      program = resolve_fixture( "DiamandDeps" ).program

      # Main links Auth, Database, Core; Auth and Database both link Core.
      # Post-order: Core (dependency of Auth) first, then Auth, then Database.
      module_names( program ).should eq( [ "Core", "Auth", "Database" ] )
    end

    it "reports a circular module dependency (C0010)" do
      error = expect_raises( Volt::Frontend::CompilationError ) do
        resolve_fixture( "CycleDeps" )
      end
      error.bag.diagnostics.map( &.code ).should contain( "C0010" )
    end

    it "reports @[Link] to a module missing from the manifest (C0009)" do
      dir = File.tempname( "volt-resolver-spec" )
      Dir.mkdir_p( File.join( dir, "Source" ) )
      begin
        manifest_source = <<-VOLT
        circuit "App"
        {
          runtime "0.1.0"
          entrypoint "Source/Main.vl"
        }
        VOLT
        File.write( File.join( dir, "Project.vl" ), manifest_source )
        File.write( File.join( dir, "Source", "Main.vl" ), %(@[Link("Nope")]\nx = 1\n) )

        manifest = Volt::Circuit.load( dir )
        resolver = Volt::Circuit::Resolver.new( manifest )
        error = expect_raises( Volt::Frontend::CompilationError ) do
          resolver.resolve
        end
        error.bag.diagnostics.map( &.code ).should contain( "C0009" )
        resolver.sources.keys.should contain( File.join( dir, "Source", "Main.vl" ) )
      ensure
        FileUtils.rm_rf( dir )
      end
    end

  end


end
