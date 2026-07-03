require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec

  extend self

  # Helper: strip ANSI escape codes so we can inspect raw text content.
  # Removes sequences of the form ESC [ ... m (SGR codes emitted by colorize).
  def strip_ansi( s : String ) : String
    s.gsub( /\e\[[0-9;]*m/, "" )
  end


  describe "Volt::REPL::REPLSyntaxHighlighter" do


    # ---------- empty source ----------

    it "returns an empty string when given an empty source" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "" )
      result.should eq( "" )
    end


    # ---------- integer literals ----------

    it "highlights an integer literal and produces non-empty output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "42" )
      result.empty?.should be_false
    end

    it "preserves the integer value in the highlighted output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "42" )
      strip_ansi( result ).should eq( "42" )
    end

    it "highlights zero as an integer" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "0" )
      strip_ansi( result ).should eq( "0" )
    end


    # ---------- float literals ----------

    it "highlights a float literal and produces non-empty output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "3.14" )
      result.empty?.should be_false
    end

    it "preserves the float value in the highlighted output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "3.14" )
      strip_ansi( result ).should eq( "3.14" )
    end


    # ---------- string literals ----------

    it "highlights a double-quoted string literal and produces non-empty output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "\"hello\"" )
      result.empty?.should be_false
    end

    it "preserves the string content in the highlighted output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "\"hello\"" )
      strip_ansi( result ).includes?( "hello" ).should be_true
    end


    # ---------- boolean literals ----------

    it "highlights 'true' and produces non-empty output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "true" )
      result.empty?.should be_false
    end

    it "preserves 'true' in the highlighted output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "true" )
      strip_ansi( result ).should eq( "true" )
    end

    it "highlights 'false' and produces non-empty output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "false" )
      result.empty?.should be_false
    end

    it "preserves 'false' in the highlighted output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "false" )
      strip_ansi( result ).should eq( "false" )
    end


    # ---------- nil literal ----------

    it "highlights 'nil' and produces non-empty output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "nil" )
      result.empty?.should be_false
    end

    it "preserves 'nil' in the highlighted output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "nil" )
      strip_ansi( result ).should eq( "nil" )
    end


    # ---------- identifiers ----------

    it "highlights an identifier and produces non-empty output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "foo" )
      result.empty?.should be_false
    end

    it "preserves the identifier name in the highlighted output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "foo" )
      strip_ansi( result ).should eq( "foo" )
    end

    it "highlights identifiers with trailing punctuation (foo?, bar!)" do
      result_q = Volt::REPL::REPLSyntaxHighlighter.highlight( "foo?" )
      strip_ansi( result_q ).should eq( "foo?" )

      result_b = Volt::REPL::REPLSyntaxHighlighter.highlight( "bar!" )
      strip_ansi( result_b ).should eq( "bar!" )
    end


    # ---------- keywords ----------

    it "highlights the 'def' keyword and produces non-empty output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "def" )
      result.empty?.should be_false
    end

    it "preserves 'def' in the highlighted output" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "def" )
      strip_ansi( result ).should eq( "def" )
    end

    it "highlights 'class' as a keyword" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "class" )
      strip_ansi( result ).should eq( "class" )
    end

    it "highlights 'if' as a keyword" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "if" )
      strip_ansi( result ).should eq( "if" )
    end

    it "highlights 'return' as a keyword" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "return" )
      strip_ansi( result ).should eq( "return" )
    end

    it "highlights 'end' as a keyword" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "end" )
      strip_ansi( result ).should eq( "end" )
    end


    # ---------- source preservation ----------

    it "preserves all source characters after stripping ANSI codes (int)" do
      source = "42"
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( source )
      strip_ansi( result ).should eq( source )
    end

    it "preserves all source characters after stripping ANSI codes (float)" do
      source = "3.14"
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( source )
      strip_ansi( result ).should eq( source )
    end

    it "preserves whitespace between tokens" do
      source = "foo bar"
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( source )
      strip_ansi( result ).should eq( source )
    end

    it "preserves newlines between tokens" do
      source = "foo\nbar"
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( source )
      strip_ansi( result ).includes?( "foo" ).should be_true
      strip_ansi( result ).includes?( "bar" ).should be_true
    end


    # ---------- mixed source ----------

    it "highlights a multi-token expression and preserves content" do
      source = "def add x y\n  x + y\nend"
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( source )
      plain  = strip_ansi( result )
      plain.includes?( "def" ).should be_true
      plain.includes?( "add" ).should be_true
      plain.includes?( "end" ).should be_true
    end

    it "highlights a source with integers and identifiers together" do
      source = "x = 10"
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( source )
      plain  = strip_ansi( result )
      plain.includes?( "x" ).should be_true
      plain.includes?( "10" ).should be_true
    end

    it "highlights a source mixing booleans, nil and identifiers" do
      source = "a = true\nb = false\nc = nil"
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( source )
      plain  = strip_ansi( result )
      plain.includes?( "true" ).should be_true
      plain.includes?( "false" ).should be_true
      plain.includes?( "nil" ).should be_true
    end


    # ---------- idempotency / length ----------

    it "calling highlight twice yields the same plain text (idempotent on content)" do
      source  = "42 + foo"
      first   = strip_ansi( Volt::REPL::REPLSyntaxHighlighter.highlight( source ) )
      second  = strip_ansi( Volt::REPL::REPLSyntaxHighlighter.highlight( source ) )
      first.should eq( second )
    end

    it "the stripped output length equals the source length for a simple int" do
      source = "99"
      result = strip_ansi( Volt::REPL::REPLSyntaxHighlighter.highlight( source ) )
      result.size.should eq( source.size )
    end

    it "the stripped output length equals the source length for a keyword" do
      source = "while"
      result = strip_ansi( Volt::REPL::REPLSyntaxHighlighter.highlight( source ) )
      result.size.should eq( source.size )
    end


    # ---------- edge cases ----------

    it "highlights source composed only of whitespace (no tokens)" do
      source = "   "
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( source )
      # Whitespace between/after tokens is appended verbatim - should survive.
      strip_ansi( result ).should eq( source )
    end

    it "handles a single newline without crashing" do
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( "\n" )
      strip_ansi( result ).should eq( "\n" )
    end

    it "handles a large integer literal without crashing" do
      source = "9999999999"
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( source )
      strip_ansi( result ).should eq( source )
    end

    it "handles multiple keywords on separate lines" do
      source = "if\nelse\nend"
      result = Volt::REPL::REPLSyntaxHighlighter.highlight( source )
      plain  = strip_ansi( result )
      plain.includes?( "if" ).should be_true
      plain.includes?( "else" ).should be_true
      plain.includes?( "end" ).should be_true
    end


  end


end
