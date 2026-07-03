require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec


  describe "Volt::REPL::REPLLineGuard.incomplete?" do

    # -----------------------------------------------------------------------
    # Cas nominaux : expressions completes (attendu: false)
    # -----------------------------------------------------------------------

    it "returns false for a simple integer literal" do
      Volt::REPL::REPLLineGuard.incomplete?( "42" ).should be_false
    end

    it "returns false for a simple float literal" do
      Volt::REPL::REPLLineGuard.incomplete?( "3.14" ).should be_false
    end

    it "returns false for a simple string literal" do
      Volt::REPL::REPLLineGuard.incomplete?( "\"hello\"" ).should be_false
    end

    it "returns false for a simple identifier" do
      Volt::REPL::REPLLineGuard.incomplete?( "foo" ).should be_false
    end

    it "returns false for a simple assignment" do
      Volt::REPL::REPLLineGuard.incomplete?( "x = 5" ).should be_false
    end

    it "returns false for an arithmetic expression" do
      Volt::REPL::REPLLineGuard.incomplete?( "1 + 2 * 3" ).should be_false
    end

    it "returns false for a method call with no arguments" do
      Volt::REPL::REPLLineGuard.incomplete?( "foo()" ).should be_false
    end

    it "returns false for a method call with arguments" do
      Volt::REPL::REPLLineGuard.incomplete?( "foo(1, 2)" ).should be_false
    end

    it "returns false for a complete array literal" do
      Volt::REPL::REPLLineGuard.incomplete?( "[1, 2, 3]" ).should be_false
    end

    it "returns false for a complete brace literal" do
      Volt::REPL::REPLLineGuard.incomplete?( "{ }" ).should be_false
    end

    it "returns false for an empty string" do
      Volt::REPL::REPLLineGuard.incomplete?( "" ).should be_false
    end

    it "returns false for whitespace-only input" do
      Volt::REPL::REPLLineGuard.incomplete?( "   " ).should be_false
    end

    # -----------------------------------------------------------------------
    # Mots-cles ouvrants sans end : attendu true
    # -----------------------------------------------------------------------

    it "returns true for an open def without end" do
      Volt::REPL::REPLLineGuard.incomplete?( "def foo" ).should be_true
    end

    it "returns true for an open class without end" do
      Volt::REPL::REPLLineGuard.incomplete?( "class Foo" ).should be_true
    end

    it "returns true for an open struct without end" do
      Volt::REPL::REPLLineGuard.incomplete?( "struct Point" ).should be_true
    end

    it "returns true for an open mixin without end" do
      Volt::REPL::REPLLineGuard.incomplete?( "mixin Serializable" ).should be_true
    end

    it "returns true for an open module without end" do
      Volt::REPL::REPLLineGuard.incomplete?( "module MyMod" ).should be_true
    end

    it "returns true for an open if without end" do
      Volt::REPL::REPLLineGuard.incomplete?( "if x > 0" ).should be_true
    end

    it "returns true for an open unless without end" do
      Volt::REPL::REPLLineGuard.incomplete?( "unless x" ).should be_true
    end

    it "returns true for an open while without end" do
      Volt::REPL::REPLLineGuard.incomplete?( "while true" ).should be_true
    end

    it "returns true for an open until without end" do
      Volt::REPL::REPLLineGuard.incomplete?( "until false" ).should be_true
    end

    it "returns true for an open match without end" do
      Volt::REPL::REPLLineGuard.incomplete?( "match x" ).should be_true
    end

    # -----------------------------------------------------------------------
    # Blocs complets avec end : attendu false
    # -----------------------------------------------------------------------

    it "returns false for a complete def...end block" do
      src = "def foo\n  42\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    it "returns false for a complete def with parameters and return type" do
      src = "def add(a : Int64, b : Int64) -> Int64\n  a + b\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    it "returns false for a complete class...end block" do
      src = "class Foo\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    it "returns false for a complete struct...end block" do
      src = "struct Point\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    it "returns false for a complete module...end block" do
      src = "module MyMod\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    it "returns false for a complete if...end block" do
      src = "if x > 0\n  42\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    it "returns false for a complete unless...end block" do
      src = "unless x\n  0\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    it "returns false for a complete while...end block" do
      src = "while true\n  x = x + 1\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    it "returns false for a complete until...end block" do
      src = "until x == 0\n  x = x - 1\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    it "returns false for a complete match...end block" do
      src = "match x\n  when 1 then 42\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    # -----------------------------------------------------------------------
    # Delimiteurs non fermes : parentheses, brackets, braces
    # -----------------------------------------------------------------------

    it "returns true for an unclosed opening parenthesis" do
      Volt::REPL::REPLLineGuard.incomplete?( "foo(" ).should be_true
    end

    it "returns true for an unclosed opening bracket" do
      Volt::REPL::REPLLineGuard.incomplete?( "[1, 2" ).should be_true
    end

    it "returns true for an unclosed opening brace" do
      Volt::REPL::REPLLineGuard.incomplete?( "{" ).should be_true
    end

    it "returns false for balanced parentheses" do
      Volt::REPL::REPLLineGuard.incomplete?( "foo(1, 2)" ).should be_false
    end

    it "returns false for balanced brackets" do
      Volt::REPL::REPLLineGuard.incomplete?( "[1, 2]" ).should be_false
    end

    it "returns false for balanced braces" do
      Volt::REPL::REPLLineGuard.incomplete?( "{ x: 1 }" ).should be_false
    end

    it "returns true for multiple unclosed opening parentheses" do
      Volt::REPL::REPLLineGuard.incomplete?( "foo( bar(" ).should be_true
    end

    # -----------------------------------------------------------------------
    # Imbrication (nesting) : plusieurs niveaux ouverts
    # -----------------------------------------------------------------------

    it "returns true for nested def and if both unclosed" do
      src = "def foo\n  if true\n"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_true
    end

    it "returns true for a def with inner if closed but outer def still open" do
      src = "def foo\n  if true\n    42\n  end\n"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_true
    end

    it "returns false for a def containing a complete if block" do
      src = "def foo\n  if true\n    42\n  end\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    it "returns true for a class containing an open def" do
      src = "class Foo\n  def bar\n"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_true
    end

    it "returns false for a class with a complete def inside" do
      src = "class Foo\n  def bar\n    0\n  end\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_false
    end

    # -----------------------------------------------------------------------
    # Cas limite : surcharge de end (plus de end que d'ouvertures)
    # indent_level devient negatif => (indent_level > 0) est false
    # -----------------------------------------------------------------------

    it "returns false when there are more ends than open keywords" do
      Volt::REPL::REPLLineGuard.incomplete?( "end end" ).should be_false
    end

    it "returns false for a lone end keyword" do
      Volt::REPL::REPLLineGuard.incomplete?( "end" ).should be_false
    end

    # -----------------------------------------------------------------------
    # Robustesse : la clause rescue doit retourner false sur erreur lexer
    # -----------------------------------------------------------------------

    it "returns false for malformed input that causes a lexer error (rescue clause)" do
      result = Volt::REPL::REPLLineGuard.incomplete?( "\x00\x01\x02" )
      result.should be_false
    end

    # -----------------------------------------------------------------------
    # Combinaisons : parentheses + mots-cles
    # -----------------------------------------------------------------------

    it "returns true for an open def with an unclosed parenthesis in signature" do
      Volt::REPL::REPLLineGuard.incomplete?( "def foo(a : Int64, b : Int64" ).should be_true
    end

    it "returns true for an if with an unclosed parenthesis in condition" do
      Volt::REPL::REPLLineGuard.incomplete?( "if foo(" ).should be_true
    end

    it "returns true for an unclosed bracket inside an otherwise balanced def...end" do
      # Le def est ferme mais un bracket reste ouvert -> global incomplet
      src = "def foo\n  x = [1, 2\nend"
      Volt::REPL::REPLLineGuard.incomplete?( src ).should be_true
    end

    # -----------------------------------------------------------------------
    # Mots-cles NON ouvrants (ne doivent pas incrementer indent_level)
    # -----------------------------------------------------------------------

    it "returns false for standalone 'return' keyword" do
      Volt::REPL::REPLLineGuard.incomplete?( "return 42" ).should be_false
    end

    it "returns false for standalone 'break' keyword" do
      Volt::REPL::REPLLineGuard.incomplete?( "break" ).should be_false
    end

    it "returns false for standalone 'next' keyword" do
      Volt::REPL::REPLLineGuard.incomplete?( "next" ).should be_false
    end

    it "returns false for the boolean literal true" do
      Volt::REPL::REPLLineGuard.incomplete?( "true" ).should be_false
    end

    it "returns false for the boolean literal false" do
      Volt::REPL::REPLLineGuard.incomplete?( "false" ).should be_false
    end

    it "returns false for the nil literal" do
      Volt::REPL::REPLLineGuard.incomplete?( "nil" ).should be_false
    end

  end


end
