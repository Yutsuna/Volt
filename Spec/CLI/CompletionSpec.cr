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

    it "stays silent when completing after a receiver dot" do
      session = Volt::REPL::REPLSession.new
      session.evaluate( "myvar = 1" )

      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::SessionSymbolProvider.new( session ) ] of Volt::CLI::ACompletionProvider )
      engine.complete( "myvar.", 6 ).should be_nil
    end

  end


  describe "Volt::CLI::KeywordProvider" do

    it "stays silent when completing after a receiver dot" do
      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::KeywordProvider.new ] of Volt::CLI::ACompletionProvider )
      engine.complete( "myvar.", 6 ).should be_nil
    end

  end


  describe "Volt::CLI::MemberCompletionProvider" do

    it "completes String methods after a string-valued receiver" do
      session = Volt::REPL::REPLSession.new
      session.evaluate( %(str = "Léo") )

      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::MemberCompletionProvider.new( session ) ] of Volt::CLI::ACompletionProvider )

      result = engine.complete( "str.", 4 )
      result.should_not be_nil
      candidates = result.not_nil!.candidates
      candidates.size.should be > 0
      candidates.all? { |c| c.kind == :method }.should be_true
    end

    it "completes Int methods after an integer-valued receiver" do
      session = Volt::REPL::REPLSession.new
      session.evaluate( "a = 10" )

      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::MemberCompletionProvider.new( session ) ] of Volt::CLI::ACompletionProvider )

      result = engine.complete( "a.", 2 )
      result.should_not be_nil
      result.not_nil!.candidates.size.should be > 0
    end

    it "filters member completions by the typed prefix" do
      session = Volt::REPL::REPLSession.new
      session.evaluate( "a = 10" )

      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::MemberCompletionProvider.new( session ) ] of Volt::CLI::ACompletionProvider )

      result = engine.complete( "a.", 2 )
      all_labels = result.not_nil!.candidates.map( &.label )
      picked = all_labels.first

      filtered = engine.complete( "a.#{picked[ 0, 1 ]}", 3 )
      filtered.not_nil!.candidates.map( &.label ).should contain picked
    end

    it "resolves methods inherited through mixins, like inspect" do
      session = Volt::REPL::REPLSession.new
      session.evaluate( "a = 10" )

      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::MemberCompletionProvider.new( session ) ] of Volt::CLI::ACompletionProvider )

      labels = engine.complete( "a.", 2 ).not_nil!.candidates.map( &.label )
      labels.should contain "inspect"
      labels.should contain "to_string"
    end

    it "never offers operator methods after a dot" do
      session = Volt::REPL::REPLSession.new
      session.evaluate( %(str = "Léo") )

      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::MemberCompletionProvider.new( session ) ] of Volt::CLI::ACompletionProvider )

      labels = engine.complete( "str.", 4 ).not_nil!.candidates.map( &.label )
      labels.should_not contain "+"
      labels.should_not contain "=="
      labels.should_not contain "=~"
    end

    it "never offers constructors or destructors" do
      session = Volt::REPL::REPLSession.new
      session.evaluate( %(str = "Léo") )

      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::MemberCompletionProvider.new( session ) ] of Volt::CLI::ACompletionProvider )

      labels = engine.complete( "str.", 4 ).not_nil!.candidates.map( &.label )
      labels.any?( &.starts_with?( "initialize" ) ).should be_false
      labels.should_not contain "finalize"
    end

    it "offers overloads once, as the bare name without arity" do
      session = Volt::REPL::REPLSession.new
      session.evaluate( <<-VOLT )
        struct A
          def b -> Void
          end
          def b( a : Int32 ) -> Void
          end
        end
        VOLT
      session.evaluate( "x = A.new" )

      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::MemberCompletionProvider.new( session ) ] of Volt::CLI::ACompletionProvider )

      labels = engine.complete( "x.b", 3 ).not_nil!.candidates.map( &.label )
      labels.count( "b" ).should eq 1
      labels.any?( &.includes?( '/' ) ).should be_false
    end

    it "returns nothing for an unknown receiver" do
      session = Volt::REPL::REPLSession.new
      engine = Volt::CLI::CompletionEngine.new( [ Volt::CLI::MemberCompletionProvider.new( session ) ] of Volt::CLI::ACompletionProvider )
      engine.complete( "nope.", 5 ).should be_nil
    end

  end


end
