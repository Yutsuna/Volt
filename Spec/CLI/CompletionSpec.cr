require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec


  describe "Volt::CLI::CompletionEngine" do

    it "extracts the word under the cursor" do
      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::KeywordProvider.new ] of Volt::CLI::ACompletionProvider )
      result = engine.complete( "x = wh", 6 )
      result.should_not be_nil
      result.not_nil!.replace_start.should eq 4
      result.not_nil!.replace_end.should eq 6
    end

    it "returns nil when there is no word under the cursor" do
      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::KeywordProvider.new ] of Volt::CLI::ACompletionProvider )
      engine.complete( "x = ", 4 ).should be_nil
    end

  end


  describe "Volt::CLI::KeywordProvider" do

    it "completes language keywords" do
      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::KeywordProvider.new ] of Volt::CLI::ACompletionProvider )
      result = engine.complete( "wh", 2 )
      labels = result.not_nil!.candidates.map( &.label )
      labels.should contain "while"
    end

  end


  describe "Volt::CLI::BuiltinCommandProvider" do

    it "completes builtin commands after a leading colon" do
      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::BuiltinCommandProvider.new ] of Volt::CLI::ACompletionProvider )
      result = engine.complete( ":he", 3 )
      labels = result.not_nil!.candidates.map( &.label )
      labels.should contain ":help"
    end

    it "does not offer builtins outside a colon command" do
      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::BuiltinCommandProvider.new ] of Volt::CLI::ACompletionProvider )
      engine.complete( "he", 2 ).should be_nil
    end

  end


  describe "Volt::CLI::SessionSymbolProvider" do

    it "completes variables and functions defined in the session" do
      session = Volt::REPL::REPLSession.new
      session.evaluate( "myvar = 1" )
      session.evaluate( "def myfunc -> Int64; 1; end" )

      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::SessionSymbolProvider.new( session ) ] of Volt::CLI::ACompletionProvider )

      result = engine.complete( "myv", 3 )
      result.not_nil!.candidates.map( &.label ).should contain "myvar"

      result = engine.complete( "myf", 3 )
      result.not_nil!.candidates.map( &.label ).should contain "myfunc"
    end

  end


end
