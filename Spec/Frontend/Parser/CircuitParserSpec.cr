require "spec"
require "../../../Source/Volt/__all__"


module Volt::Spec


  describe "Volt::Frontend::Parser (circuit)" do

    it "parses a circuit block using braces" do
      source = <<-VOLT
      circuit "TwoDepsApp"
      {
        runtime "0.1.0"
        entrypoint "Source/Main.vl"

        modules(
          "Models"      => "Source/Models",
          "Components"  => "Source/Components",
        )
      }
      VOLT

      program = Volt::Frontend.parse( source, "<test>" )
      program.nodes.size.should eq( 1 )

      decl = program.nodes.first.as( Volt::Frontend::CircuitDecl )
      decl.name.should eq( "TwoDepsApp" )
      decl.runtime.should eq( "0.1.0" )
      decl.entrypoint.should eq( "Source/Main.vl" )

      modules = decl.modules.not_nil!
      modules.pairs.size.should eq( 2 )

      key0 = modules.pairs[ 0 ][ 0 ].as( Volt::Frontend::StringLit )
      val0 = modules.pairs[ 0 ][ 1 ].as( Volt::Frontend::StringLit )
      key0.value.should eq( "Models" )
      val0.value.should eq( "Source/Models" )

      key1 = modules.pairs[ 1 ][ 0 ].as( Volt::Frontend::StringLit )
      val1 = modules.pairs[ 1 ][ 1 ].as( Volt::Frontend::StringLit )
      key1.value.should eq( "Components" )
      val1.value.should eq( "Source/Components" )
    end

    it "parses a circuit block using do/end (equivalent to braces)" do
      source = <<-VOLT
      circuit "DoApp" do
        runtime "0.1.0"
        entrypoint "Source/Main.vl"
        modules(
          "Foo" => "Source/Foo",
        )
      end
      VOLT

      program = Volt::Frontend.parse( source, "<test>" )
      decl = program.nodes.first.as( Volt::Frontend::CircuitDecl )
      decl.name.should eq( "DoApp" )
      decl.modules.not_nil!.pairs.size.should eq( 1 )
    end

    it "tolerates a trailing comma in the modules hash" do
      source = <<-VOLT
      circuit "App" {
        modules(
          "A" => "Source/A",
        )
      }
      VOLT

      program = Volt::Frontend.parse( source, "<test>" )
      decl = program.nodes.first.as( Volt::Frontend::CircuitDecl )
      decl.modules.not_nil!.pairs.size.should eq( 1 )
    end

    it "parses a circuit with no runtime/entrypoint/modules entries" do
      source = <<-VOLT
      circuit "Empty" {
      }
      VOLT

      program = Volt::Frontend.parse( source, "<test>" )
      decl = program.nodes.first.as( Volt::Frontend::CircuitDecl )
      decl.name.should eq( "Empty" )
      decl.runtime.should be_nil
      decl.entrypoint.should be_nil
      decl.modules.should be_nil
    end

    it "raises a diagnostic when the opening `{` or `do` is missing" do
      source = %(circuit "Broken" runtime "0.1.0")

      expect_raises( Volt::Frontend::CompilationError ) do
        Volt::Frontend.parse( source, "<test>" )
      end
    end

    it "raises a diagnostic for an unknown circuit entry" do
      source = <<-VOLT
      circuit "App" {
        battery "json"
      }
      VOLT

      expect_raises( Volt::Frontend::CompilationError ) do
        Volt::Frontend.parse( source, "<test>" )
      end
    end

    it "rejects string interpolation inside circuit manifest values" do
      source = <<-VOLT
      name = "hack"
      circuit "\#{name}" {
      }
      VOLT

      expect_raises( Volt::Frontend::CompilationError ) do
        Volt::Frontend.parse( source, "<test>" )
      end
    end

    it "rejects string interpolation inside a module path" do
      source = <<-VOLT
      x = "y"
      circuit "App" {
        modules(
          "A" => "\#{x}",
        )
      }
      VOLT

      expect_raises( Volt::Frontend::CompilationError ) do
        Volt::Frontend.parse( source, "<test>" )
      end
    end

    it "requires modules entries to use `=>` between key and value" do
      source = <<-VOLT
      circuit "App" {
        modules(
          "A" "Source/A",
        )
      }
      VOLT

      expect_raises( Volt::Frontend::CompilationError ) do
        Volt::Frontend.parse( source, "<test>" )
      end
    end

  end


end
