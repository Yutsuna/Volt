require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec


  describe "REPL::REPLSession" do


    # -------------------------------------------------------------------------
    # initialize
    # -------------------------------------------------------------------------

    it "initialize: history is empty on a fresh session" do
      session = REPL::REPLSession.new
      session.history.should be_empty
    end


    # -------------------------------------------------------------------------
    # clear
    # -------------------------------------------------------------------------

    it "clear: empties the history that was accumulated" do
      session = REPL::REPLSession.new
      # Populate history by evaluating a declaration
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      session.evaluate( "x = 5", stdout_io, stderr_io )
      session.history.should_not be_empty

      session.clear
      session.history.should be_empty
    end

    it "clear: is a no-op on an already empty session" do
      session = REPL::REPLSession.new
      session.clear
      session.history.should be_empty
    end


    # -------------------------------------------------------------------------
    # evaluate - ok? on valid input
    # -------------------------------------------------------------------------

    it "evaluate: a simple integer literal returns ok? == true" do
      session = REPL::REPLSession.new
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      result = session.evaluate( "42", stdout_io, stderr_io )
      result.ok?.should be_true
    end

    it "evaluate: a simple boolean literal returns ok? == true" do
      session = REPL::REPLSession.new
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      result = session.evaluate( "true", stdout_io, stderr_io )
      result.ok?.should be_true
    end

    it "evaluate: a simple string literal returns ok? == true" do
      session = REPL::REPLSession.new
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      result = session.evaluate( "\"hello\"", stdout_io, stderr_io )
      result.ok?.should be_true
    end


    # -------------------------------------------------------------------------
    # evaluate - ok? on invalid input
    # -------------------------------------------------------------------------

    it "evaluate: an undefined variable reference returns ok? == false" do
      session = REPL::REPLSession.new
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      result = session.evaluate( "undefined_xyz", stdout_io, stderr_io )
      result.ok?.should be_false
    end

    it "evaluate: syntactically invalid input returns ok? == false" do
      session = REPL::REPLSession.new
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      # An isolated operator with no operands is a parse/analyse error
      result = session.evaluate( "* +", stdout_io, stderr_io )
      result.ok?.should be_false
    end

    it "evaluate: an invalid input does not persist diagnostics as nil (bag present)" do
      session = REPL::REPLSession.new
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      result = session.evaluate( "undefined_xyz", stdout_io, stderr_io )
      result.diagnostics.should_not be_nil
    end


    # -------------------------------------------------------------------------
    # evaluate - history saving (declaration vs expression)
    # -------------------------------------------------------------------------

    it "evaluate: a variable declaration is saved in history" do
      session = REPL::REPLSession.new
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      result = session.evaluate( "x = 5", stdout_io, stderr_io )
      result.definition_saved?.should be_true
      session.history.includes?( "x = 5" ).should be_true
    end

    it "evaluate: a simple expression is NOT saved in history" do
      session = REPL::REPLSession.new
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      result = session.evaluate( "42", stdout_io, stderr_io )
      result.definition_saved?.should be_false
      session.history.should be_empty
    end

    it "evaluate: a failed evaluation is NOT saved in history" do
      session = REPL::REPLSession.new
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      session.evaluate( "undefined_xyz", stdout_io, stderr_io )
      session.history.should be_empty
    end

    it "evaluate: multiple declarations accumulate in history in order" do
      session = REPL::REPLSession.new
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      session.evaluate( "x = 1", stdout_io, stderr_io )
      session.evaluate( "y = 2", stdout_io, stderr_io )
      session.history.size.should eq( 2 )
      session.history[0].should eq( "x = 1" )
      session.history[1].should eq( "y = 2" )
    end


    # -------------------------------------------------------------------------
    # evaluate - context accumulation across calls
    # -------------------------------------------------------------------------

    it "evaluate: a variable defined in one call is usable in the next call" do
      session = REPL::REPLSession.new
      stdout_io1 = IO::Memory.new
      stderr_io1 = IO::Memory.new
      r1 = session.evaluate( "x = 10", stdout_io1, stderr_io1 )
      r1.ok?.should be_true

      stdout_io2 = IO::Memory.new
      stderr_io2 = IO::Memory.new
      r2 = session.evaluate( "x", stdout_io2, stderr_io2 )
      r2.ok?.should be_true
    end

    it "evaluate: after clear, a previously defined variable is no longer in scope" do
      session = REPL::REPLSession.new
      stdout_io = IO::Memory.new
      stderr_io = IO::Memory.new
      session.evaluate( "x = 10", stdout_io, stderr_io )
      session.clear

      stdout_io2 = IO::Memory.new
      stderr_io2 = IO::Memory.new
      r2 = session.evaluate( "x", stdout_io2, stderr_io2 )
      # 'x' is gone: should be an error
      r2.ok?.should be_false
    end


  end


end
