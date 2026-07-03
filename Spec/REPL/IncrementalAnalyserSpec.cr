require "spec"
require "../../Source/Volt/__all__"


describe "REPL::IncrementalState & Frontend::IncrementalAnalyser" do


  it "accumulates top-level global variables across runs" do
    state = Volt::REPL::IncrementalState.new

    # Run 1: define x = 42
    p1 = Volt::Frontend.parse( "x = 42", "<repl>" )
    analyser1 = Volt::Frontend::IncrementalAnalyser.new( p1, state )
    typed1 = analyser1.analyse

    state.top_level_globals.has_key?( "x" ).should be_true
    state.top_level_globals[ "x" ].integer?.should be_true

    # Run 2: use x in y = x + 1
    p2 = Volt::Frontend.parse( "y = x + 1", "<repl>" )
    analyser2 = Volt::Frontend::IncrementalAnalyser.new( p2, state )
    typed2 = analyser2.analyse

    state.top_level_globals.has_key?( "y" ).should be_true
    state.top_level_globals[ "y" ].integer?.should be_true
  end

  it "accumulates function signatures and user-defined types across runs" do
    state = Volt::REPL::IncrementalState.new

    # Run 1: define class Foo and def bar
    p1 = Volt::Frontend.parse( "class Foo; end; def bar -> Foo; Foo.new; end", "<repl>" )
    analyser1 = Volt::Frontend::IncrementalAnalyser.new( p1, state )
    typed1 = analyser1.analyse

    state.types.has_key?( "Foo" ).should be_true
    state.signatures.has_key?( "bar" ).should be_true

    # Run 2: use Foo and bar
    p2 = Volt::Frontend.parse( "f : Foo = bar", "<repl>" )
    analyser2 = Volt::Frontend::IncrementalAnalyser.new( p2, state )
    typed2 = analyser2.analyse

    state.top_level_globals.has_key?( "f" ).should be_true
  end


end
