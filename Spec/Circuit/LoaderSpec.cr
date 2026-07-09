require "spec"
require "file_utils"
require "../../Source/Volt/__all__"


module Volt::Spec

  CIRCUIT_FIXTURES = File.expand_path( File.join( __DIR__, "..", "..", "Samples", "Tests", "Circuits" ) )


  # Builds a throwaway project directory (manifest + files) for invalid-manifest
  # cases, and cleans it up after the block.
  private def self.with_project( manifest : String, files : Array( String ) = [] of String, & : String -> )
    dir = File.tempname( "volt-circuit-spec" )
    Dir.mkdir_p( dir )
    begin
      File.write( File.join( dir, "Project.vl" ), manifest )
      files.each do |rel|
        path = File.join( dir, rel )
        Dir.mkdir_p( File.dirname( path ) )
        File.write( path, "" )
      end
      yield dir
    ensure
      FileUtils.rm_rf( dir )
    end
  end

  private def self.expect_circuit_error( code : String, manifest : String, files : Array( String ) = [] of String )
    with_project( manifest, files ) do |dir|
      error = expect_raises( Volt::Frontend::CompilationError ) do
        Volt::Circuit.load( dir )
      end
      error.bag.diagnostics.map( &.code ).should contain( code )
    end
  end


  describe "Volt::Circuit.load" do

    it "loads the TwoDeps fixture manifest" do
      manifest = Volt::Circuit.load( File.join( CIRCUIT_FIXTURES, "TwoDeps" ) )

      manifest.name.should eq( "TwoDepsApp" )
      manifest.runtime.should eq( "0.1.0" )
      manifest.entrypoint.should eq( "Source/Main.vl" )
      manifest.modules.should eq( {
        "Models"     => "Source/Models",
        "Components" => "Source/Components",
      } )
      manifest.root.should eq( File.join( CIRCUIT_FIXTURES, "TwoDeps" ) )
      File.file?( manifest.entrypoint_path ).should be_true
      manifest.module_path( "Models" ).should eq( File.join( CIRCUIT_FIXTURES, "TwoDeps", "Source", "Models" ) )
      manifest.module_path( "Nope" ).should be_nil
    end

    it "loads the DiamandDeps fixture manifest" do
      manifest = Volt::Circuit.load( File.join( CIRCUIT_FIXTURES, "DiamandDeps" ) )

      manifest.name.should eq( "DiamonApp" )
      manifest.modules.keys.sort!.should eq( [ "Auth", "Core", "Database" ] )
    end

    it "accepts a direct path to the Project.vl file" do
      manifest = Volt::Circuit.load( File.join( CIRCUIT_FIXTURES, "TwoDeps", "Project.vl" ) )
      manifest.name.should eq( "TwoDepsApp" )
    end

    it "fails when the manifest file does not exist (C0001)" do
      error = expect_raises( Volt::Frontend::CompilationError ) do
        Volt::Circuit.load( "/nonexistent/Project.vl" )
      end
      error.bag.diagnostics.map( &.code ).should contain( "C0001" )
    end

    it "fails when the manifest has no circuit block (C0002)" do
      expect_circuit_error( "C0002", "x = 1\n" )
    end

    it "fails when the manifest has two circuit blocks (C0003)" do
      manifest = <<-VOLT
      circuit "A" {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"
      }
      circuit "B" {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"
      }
      VOLT
      expect_circuit_error( "C0003", manifest, [ "Source/Main.vl" ] )
    end

    it "fails when runtime or entrypoint entries are missing (C0004)" do
      with_project( %(circuit "App" {\n}\n) ) do |dir|
        error = expect_raises( Volt::Frontend::CompilationError ) do
          Volt::Circuit.load( dir )
        end
        error.bag.diagnostics.count( &.code.== "C0004" ).should eq( 2 )
      end
    end

    it "fails when the entrypoint does not exist on disk (C0005)" do
      manifest = <<-VOLT
      circuit "App" {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"
      }
      VOLT
      expect_circuit_error( "C0005", manifest )
    end

    it "fails on a duplicated module name (C0006)" do
      manifest = <<-VOLT
      circuit "App" {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"
        modules(
          "Models" => "Source/Models",
          "Models" => "Source/Other",
        )
      }
      VOLT
      expect_circuit_error( "C0006", manifest,
                            [ "Source/Main.vl", "Source/Models/.keep", "Source/Other/.keep" ] )
    end

    it "fails when a module path is not an existing directory (C0007)" do
      manifest = <<-VOLT
      circuit "App" {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"
        modules(
          "Models" => "Source/Missing",
        )
      }
      VOLT
      expect_circuit_error( "C0007", manifest, [ "Source/Main.vl" ] )
    end

    it "rejects module paths escaping the project root via `../` (C0008)" do
      manifest = <<-VOLT
      circuit "App" {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"
        modules(
          "Evil" => "../Outside",
        )
      }
      VOLT
      expect_circuit_error( "C0008", manifest, [ "Source/Main.vl" ] )
    end

    it "rejects absolute module paths (C0008)" do
      manifest = <<-VOLT
      circuit "App" {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"
        modules(
          "Evil" => "/etc",
        )
      }
      VOLT
      expect_circuit_error( "C0008", manifest, [ "Source/Main.vl" ] )
    end

    it "rejects an entrypoint escaping the project root (C0008)" do
      manifest = <<-VOLT
      circuit "App" {
        runtime "0.1.0"
        entrypoint "../Elsewhere/Main.vl"
      }
      VOLT
      expect_circuit_error( "C0008", manifest )
    end

    it "allows sneaky-but-confined paths like `Source/../Source/Models`" do
      manifest = <<-VOLT
      circuit "App" {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"
        modules(
          "Models" => "Source/../Source/Models",
        )
      }
      VOLT
      with_project( manifest, [ "Source/Main.vl", "Source/Models/.keep" ] ) do |dir|
        Volt::Circuit.load( dir ).modules[ "Models" ].should eq( "Source/../Source/Models" )
      end
    end

  end


end
