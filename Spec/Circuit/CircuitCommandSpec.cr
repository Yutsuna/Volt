require "spec"
require "file_utils"
require "../../Source/Volt/__all__"


module Volt::Spec

  # Builds a throwaway project directory (`Source/` tree + optional
  # `Project.vl`) and cleans it up after the block.
  private def self.with_scratch_project(files : Array( String ) = [] of String,
                                        manifest : String? = nil, & : String -> )
    dir = File.tempname( "volt-circuit-cmd-spec" )
    Dir.mkdir_p( dir )
    begin
      files.each do |rel|
        path = File.join( dir, rel )
        Dir.mkdir_p( File.dirname( path ) )
        File.write( path, "" )
      end
      File.write( File.join( dir, "Project.vl" ), manifest ) if manifest
      yield dir
    ensure
      FileUtils.rm_rf( dir )
    end
  end

  private def self.run_circuit(dir : String) : Int32
    Volt::CLI.run( [ "circuit", "-d", dir ] )
  end


  describe "volt circuit" do

    it "creates a Project.vl from scratch, scanning Source/ for module directories" do
      with_scratch_project( [
        "Source/Main.vl",
        "Source/Models/User.vl",
        "Source/Components/Logger.vl",
      ] ) do |dir|
        run_circuit( dir ).should eq( Volt::CLI::EXIT_SUCCESS )

        manifest = Volt::Circuit.load( dir )
        manifest.name.should eq( File.basename( dir ) )
        manifest.runtime.should eq( Volt::VERSION )
        manifest.entrypoint.should eq( "Source/Main.vl" )
        manifest.modules.should eq( {
          "Components" => "Source/Components",
          "Models"     => "Source/Models",
        } )
      end
    end

    it "fails clearly when Source/Main.vl is missing" do
      with_scratch_project( [ "Source/Models/User.vl" ] ) do |dir|
        run_circuit( dir ).should eq( Volt::CLI::EXIT_ERROR )
        File.file?( File.join( dir, "Project.vl" ) ).should be_false
      end
    end

    it "adds newly discovered module directories on update, preserving existing ones" do
      manifest = <<-VOLT
      circuit "App"
      {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"

        modules(
          "Models" => "Source/Models",
        )
      }
      VOLT

      with_scratch_project(
        [ "Source/Main.vl", "Source/Models/User.vl", "Source/Components/Logger.vl" ],
        manifest: manifest
      ) do |dir|
        run_circuit( dir ).should eq( Volt::CLI::EXIT_SUCCESS )

        result = Volt::Circuit.load( dir )
        result.modules.should eq( {
          "Models"     => "Source/Models",
          "Components" => "Source/Components",
        } )
      end
    end

    it "preserves a custom module mapping instead of overwriting it" do
      manifest = <<-VOLT
      circuit "App"
      {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"

        modules(
          "Models" => "Source/Custom/Wherever",
        )
      }
      VOLT

      with_scratch_project(
        [ "Source/Main.vl", "Source/Custom/Wherever/User.vl", "Source/Models/User.vl" ],
        manifest: manifest
      ) do |dir|
        run_circuit( dir ).should eq( Volt::CLI::EXIT_SUCCESS )

        # "Models" keeps its custom mapping. "Source/Custom" is discovered
        # as a new module in its own right ("Custom"); the plain
        # "Source/Models" directory scans under the same key ("Models") as
        # the custom entry, so it is skipped rather than overwriting it.
        result = Volt::Circuit.load( dir )
        result.modules.should eq( {
          "Models" => "Source/Custom/Wherever",
          "Custom" => "Source/Custom",
        } )
      end
    end

    it "keeps (and does not crash on) a module whose directory disappeared, only warning" do
      manifest = <<-VOLT
      circuit "App"
      {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"

        modules(
          "Models" => "Source/Models",
        )
      }
      VOLT

      with_scratch_project( [ "Source/Main.vl" ], manifest: manifest ) do |dir|
        run_circuit( dir ).should eq( Volt::CLI::EXIT_SUCCESS )

        content = File.read( File.join( dir, "Project.vl" ) )
        content.should contain( %("Models" => "Source/Models") )
      end
    end

    it "is idempotent: two consecutive runs produce an identical file" do
      with_scratch_project( [
        "Source/Main.vl",
        "Source/Models/User.vl",
        "Source/Components/Logger.vl",
      ] ) do |dir|
        run_circuit( dir ).should eq( Volt::CLI::EXIT_SUCCESS )
        first = File.read( File.join( dir, "Project.vl" ) )

        run_circuit( dir ).should eq( Volt::CLI::EXIT_SUCCESS )
        second = File.read( File.join( dir, "Project.vl" ) )

        second.should eq( first )
      end
    end

    it "round-trips the two real fixtures without changing their semantics" do
      fixtures_root = File.expand_path( File.join( __DIR__, "..", "..", "Samples", "Tests", "Circuits" ) )

      [ "TwoDeps", "DiamandDeps" ].each do |name|
        src = File.join( fixtures_root, name )
        dst = File.tempname( "volt-circuit-cmd-spec-fixture" )
        FileUtils.cp_r( src, dst )
        begin
          before = Volt::Circuit.load( dst )
          run_circuit( dst ).should eq( Volt::CLI::EXIT_SUCCESS )
          after = Volt::Circuit.load( dst )

          after.name.should eq( before.name )
          after.runtime.should eq( before.runtime )
          after.entrypoint.should eq( before.entrypoint )
          after.modules.should eq( before.modules )
        ensure
          FileUtils.rm_rf( dst )
        end
      end
    end

  end


end
