require "spec"
require "../../../Source/Volt/__all__"


module Volt::Spec


  describe "Volt::Frontend::TokenKind" do

    it "defines literal token kinds" do
      Volt::Frontend::TokenKind::Int.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Float.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::String.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::True.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::False.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Nil.should be_a( Volt::Frontend::TokenKind )
    end

    it "defines keyword token kinds" do
      Volt::Frontend::TokenKind::Def.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::End.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Class.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Struct.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::If.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Else.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Return.should be_a( Volt::Frontend::TokenKind )
    end

    it "defines arithmetic operator token kinds" do
      Volt::Frontend::TokenKind::Plus.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Minus.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Star.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Slash.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Percent.should be_a( Volt::Frontend::TokenKind )
    end

    it "defines comparison operator token kinds" do
      Volt::Frontend::TokenKind::EqEq.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::BangEq.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Lt.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Gt.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::LtEq.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::GtEq.should be_a( Volt::Frontend::TokenKind )
    end

    it "defines assignment operator token kinds" do
      Volt::Frontend::TokenKind::Eq.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::PlusEq.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::MinusEq.should be_a( Volt::Frontend::TokenKind )
    end

    it "defines structural token kinds" do
      Volt::Frontend::TokenKind::Dot.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Comma.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Colon.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Arrow.should be_a( Volt::Frontend::TokenKind )
    end

    it "defines delimiter token kinds" do
      Volt::Frontend::TokenKind::LParen.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::RParen.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::LBracket.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::RBracket.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::LBrace.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::RBrace.should be_a( Volt::Frontend::TokenKind )
    end

    it "defines EOF and whitespace token kinds" do
      Volt::Frontend::TokenKind::Eof.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Newline.should be_a( Volt::Frontend::TokenKind )
      Volt::Frontend::TokenKind::Error.should be_a( Volt::Frontend::TokenKind )
    end

    it "has eof? predicate" do
      Volt::Frontend::TokenKind::Eof.eof?.should be_true
      Volt::Frontend::TokenKind::Int.eof?.should be_false
    end

    it "has newline? predicate" do
      Volt::Frontend::TokenKind::Newline.newline?.should be_true
      Volt::Frontend::TokenKind::Int.newline?.should be_false
    end

  end


  describe "Volt::Frontend::Token" do

    it "initializes with kind, ptr, len, and span" do
      source = "test"
      span = Volt::Frontend::Span.new( "test.vl", 1, 1, 4, 0 )
      token = Volt::Frontend::Token.new(
        Volt::Frontend::TokenKind::Ident,
        source.to_unsafe,
        4,
        span
      )
      token.kind.should eq( Volt::Frontend::TokenKind::Ident )
      token.len.should eq( 4 )
      token.span.should eq( span )
    end

    it "returns value as String" do
      source = "hello"
      span = Volt::Frontend::Span.new( "test.vl", 1, 1, 5, 0 )
      token = Volt::Frontend::Token.new(
        Volt::Frontend::TokenKind::Ident,
        source.to_unsafe,
        5,
        span
      )
      token.value.should eq( "hello" )
    end

    it "compares value with string using value_eq?" do
      source = "test"
      span = Volt::Frontend::Span.new( "test.vl", 1, 1, 4, 0 )
      token = Volt::Frontend::Token.new(
        Volt::Frontend::TokenKind::Ident,
        source.to_unsafe,
        4,
        span
      )
      token.value_eq?( "test" ).should be_true
      token.value_eq?( "other" ).should be_false
    end

  end


end
