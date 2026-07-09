require "spec"
require "../../../Source/Volt/__all__"


module Volt::Spec


  # NOTE: `should be_a( Volt::Frontend::XxxDecl )` is avoided here on purpose:
  # it trips a Crystal 1.19.1 codegen bug (BUG: trying to downcast
  # Volt::Frontend::PointerType <- Volt::Frontend::ATypeNode). `is_a?` and
  # `.as( ... )` compile fine and assert the same thing.
  describe "Volt::Frontend::Parser (@[Link])" do

    it "parses a file-level @[Link] before a module as a LinkDecl" do
      source = <<-VOLT
      @[Link("Core")]


      module Auth
      end
      VOLT

      program = Volt::Frontend.parse( source, "<test>" )
      program.nodes.size.should eq( 2 )

      link = program.nodes[ 0 ].as( Volt::Frontend::LinkDecl )
      link.module_name.should eq( "Core" )
      program.nodes[ 1 ].is_a?( Volt::Frontend::ModuleDecl ).should be_true
    end

    it "parses @[Link] annotations before a top-level expression" do
      source = <<-VOLT
      @[Link("Models")]
      @[Link("Components")]


      x = 1
      VOLT

      program = Volt::Frontend.parse( source, "<test>" )
      program.nodes.size.should eq( 3 )
      program.nodes[ 0 ].as( Volt::Frontend::LinkDecl ).module_name.should eq( "Models" )
      program.nodes[ 1 ].as( Volt::Frontend::LinkDecl ).module_name.should eq( "Components" )
    end

    it "parses a file that ends on a lone @[Link]" do
      program = Volt::Frontend.parse( %(@[Link("Tail")]\n), "<test>" )
      program.nodes.size.should eq( 1 )
      program.nodes[ 0 ].as( Volt::Frontend::LinkDecl ).module_name.should eq( "Tail" )
    end

    it "keeps @[Link] separate from annotations attached to the next decl" do
      source = <<-VOLT
      @[Link("Models")]
      @[External("libc")]
      def puts( str : String ) -> Int32
      VOLT

      program = Volt::Frontend.parse( source, "<test>" )
      program.nodes.size.should eq( 2 )
      program.nodes[ 0 ].is_a?( Volt::Frontend::LinkDecl ).should be_true
      program.nodes[ 1 ].as( Volt::Frontend::ExternDecl ).lib.should eq( "libc" )
    end

    it "rejects @[Link] without arguments (P0010)" do
      error = expect_raises( Volt::Frontend::CompilationError ) do
        Volt::Frontend.parse( "@[Link]\nx = 1\n", "<test>" )
      end
      error.bag.diagnostics.map( &.code ).should contain( "P0010" )
    end

    it "rejects @[Link] with a non-string argument (P0010)" do
      error = expect_raises( Volt::Frontend::CompilationError ) do
        Volt::Frontend.parse( "@[Link(42)]\nx = 1\n", "<test>" )
      end
      error.bag.diagnostics.map( &.code ).should contain( "P0010" )
    end

    it "rejects @[Link] with two arguments (P0010)" do
      error = expect_raises( Volt::Frontend::CompilationError ) do
        Volt::Frontend.parse( %(@[Link("A", "B")]\nx = 1\n), "<test>" )
      end
      error.bag.diagnostics.map( &.code ).should contain( "P0010" )
    end

  end


end
