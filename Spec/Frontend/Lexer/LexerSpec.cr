require "spec"
require "../../../Source/Volt/__all__"


module Volt::Spec


  describe "Volt::Frontend::Lexer" do

    it "tokenizes empty source" do
      tokens = Volt::Frontend::Lexer.tokenize( "", "<test>" )
      tokens.size.should eq( 1 )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Eof )
    end

    it "tokenizes single identifier" do
      tokens = Volt::Frontend::Lexer.tokenize( "foo", "<test>" )
      tokens.size.should eq( 2 )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 0 ].value.should eq( "foo" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Eof )
    end

    it "tokenizes multiple identifiers separated by whitespace" do
      tokens = Volt::Frontend::Lexer.tokenize( "foo bar baz", "<test>" )
      tokens.size.should eq( 4 )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 0 ].value.should eq( "foo" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 1 ].value.should eq( "bar" )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 2 ].value.should eq( "baz" )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Eof )
    end

    it "tokenizes integer literals" do
      tokens = Volt::Frontend::Lexer.tokenize( "123 456", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Int )
      tokens[ 0 ].value.should eq( "123" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Int )
      tokens[ 1 ].value.should eq( "456" )
    end

    it "tokenizes float literals" do
      tokens = Volt::Frontend::Lexer.tokenize( "3.14 0.5", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Float )
      tokens[ 0 ].value.should eq( "3.14" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Float )
      tokens[ 1 ].value.should eq( "0.5" )
    end

    it "tokenizes string literals with double quotes" do
      tokens = Volt::Frontend::Lexer.tokenize( "\"hello\"", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::String )
      tokens[ 0 ].value.should eq( "\"hello\"" )
    end

    it "tokenizes single-quoted literals as char tokens" do
      tokens = Volt::Frontend::Lexer.tokenize( "'w'", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Char )
      tokens[ 0 ].value.should eq( "'w'" )
    end

    it "tokenizes boolean literals" do
      tokens = Volt::Frontend::Lexer.tokenize( "true false", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::True )
      tokens[ 0 ].value.should eq( "true" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::False )
      tokens[ 1 ].value.should eq( "false" )
    end

    it "tokenizes nil literal" do
      tokens = Volt::Frontend::Lexer.tokenize( "nil", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Nil )
      tokens[ 0 ].value.should eq( "nil" )
    end

    it "tokenizes arithmetic operators" do
      tokens = Volt::Frontend::Lexer.tokenize( "a + b - c * d / e % f", "<test>" )
      tokens[1].kind.should eq(Volt::Frontend::TokenKind::Plus)
      tokens[3].kind.should eq(Volt::Frontend::TokenKind::Minus)
      tokens[5].kind.should eq(Volt::Frontend::TokenKind::Star)
      tokens[7].kind.should eq(Volt::Frontend::TokenKind::Slash)
      tokens[9].kind.should eq(Volt::Frontend::TokenKind::Percent)
    end

    it "tokenizes compound assignment operators" do
      tokens = Volt::Frontend::Lexer.tokenize( "+= -= *= a /= %=", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::PlusEq )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::MinusEq )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::StarEq )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 4 ].kind.should eq( Volt::Frontend::TokenKind::SlashEq )
      tokens[ 5 ].kind.should eq( Volt::Frontend::TokenKind::PercentEq )
    end

    it "tokenizes comparison operators" do
      tokens = Volt::Frontend::Lexer.tokenize( "== != < > <= >=", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::EqEq )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::BangEq )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Lt )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Gt )
      tokens[ 4 ].kind.should eq( Volt::Frontend::TokenKind::LtEq )
      tokens[ 5 ].kind.should eq( Volt::Frontend::TokenKind::GtEq )
    end

    it "tokenizes logical operators" do
      tokens = Volt::Frontend::Lexer.tokenize( "&& || !", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::AmpAmp )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::PipePipe )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Bang )
    end

    it "tokenizes arrow operator" do
      tokens = Volt::Frontend::Lexer.tokenize( "->", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Arrow )
    end

    it "tokenizes dot and range operators" do
      tokens = Volt::Frontend::Lexer.tokenize( ". .. ...", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Dot )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::DotDot )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::DotDotDot )
    end

    it "tokenizes delimiters" do
      tokens = Volt::Frontend::Lexer.tokenize( "()[]{}", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::LParen )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::RParen )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::LBracket )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::RBracket )
      tokens[ 4 ].kind.should eq( Volt::Frontend::TokenKind::LBrace )
      tokens[ 5 ].kind.should eq( Volt::Frontend::TokenKind::RBrace )
    end

    it "tokenizes punctuation" do
      tokens = Volt::Frontend::Lexer.tokenize( ",;:?", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Comma )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Semicolon )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Colon )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Question )
    end

    it "tokenizes instance variable access" do
      tokens = Volt::Frontend::Lexer.tokenize( "@x @name", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::At )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::At )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
    end

    it "tokenizes safe navigation operator" do
      tokens = Volt::Frontend::Lexer.tokenize( "?.", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Question )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Dot )
    end

    it "tokenizes pipe operator" do
      tokens = Volt::Frontend::Lexer.tokenize( "|>", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::PipeGt )
    end

    it "tokenizes keywords" do
      tokens = Volt::Frontend::Lexer.tokenize( "def end class struct if else", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Def )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::End )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Class )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Struct )
      tokens[ 4 ].kind.should eq( Volt::Frontend::TokenKind::If )
      tokens[ 5 ].kind.should eq( Volt::Frontend::TokenKind::Else )
    end

    it "tokenizes control flow keywords" do
      tokens = Volt::Frontend::Lexer.tokenize( "while until for break next return", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::While )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Until )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::For )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Break )
      tokens[ 4 ].kind.should eq( Volt::Frontend::TokenKind::Next )
      tokens[ 5 ].kind.should eq( Volt::Frontend::TokenKind::Return )
    end

    it "tokenizes type-related keywords" do
      tokens = Volt::Frontend::Lexer.tokenize( "mixin include use module abstract", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Mixin )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Include )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Use )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Module )
      tokens[ 4 ].kind.should eq( Volt::Frontend::TokenKind::Abstract )
    end

    it "tokenizes special keywords" do
      tokens = Volt::Frontend::Lexer.tokenize( "self super raise async await", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Self_ )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Super )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Raise )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Async )
      tokens[ 4 ].kind.should eq( Volt::Frontend::TokenKind::Await )
    end

    it "tokenizes pattern matching keywords" do
      tokens = Volt::Frontend::Lexer.tokenize( "match when is as", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Match )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::When )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Is )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::As )
    end

    it "tokenizes pseudo-variables" do
      tokens = Volt::Frontend::Lexer.tokenize( "__FILE__ __LINE__ __DIR__", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::DunderFile )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::DunderLine )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::DunderDir )
    end

    it "tokenizes comments as newlines" do
      tokens = Volt::Frontend::Lexer.tokenize( "foo # comment\nbar", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Newline )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Eof )
    end

    it "tracks line numbers" do
      tokens = Volt::Frontend::Lexer.tokenize( "foo\nbar\nbaz", "<test>" )
      tokens[ 0 ].span.line.should eq( 1 )
      tokens[ 1 ].span.line.should eq( 1 )
      tokens[ 2 ].span.line.should eq( 2 )
      tokens[ 3 ].span.line.should eq( 2 )
      tokens[ 4 ].span.line.should eq( 3 )
    end

    it "tokenizes multiline source" do
      source = <<-VOLT
        def foo
          bar
        end
      VOLT
      tokens = Volt::Frontend::Lexer.tokenize( source, "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Def )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Newline )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 4 ].kind.should eq( Volt::Frontend::TokenKind::Newline )
      tokens[ 5 ].kind.should eq( Volt::Frontend::TokenKind::End )
      tokens[ 6 ].kind.should eq( Volt::Frontend::TokenKind::Eof )
    end

    it "tokenizes exponentiation operator" do
      tokens = Volt::Frontend::Lexer.tokenize( "**", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::StarStar )
    end

    it "tokenizes compound operators with &" do
      tokens = Volt::Frontend::Lexer.tokenize( "&+ &- &* &**", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::AmpPlus )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::AmpMinus )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::AmpStar )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::AmpStarStar )
    end

    it "tokenizes floor division operator" do
      tokens = Volt::Frontend::Lexer.tokenize( "a // b", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::SlashSlash )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
    end

    it "tokenizes spaceship operator" do
      tokens = Volt::Frontend::Lexer.tokenize( "<=>", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Spaceship )
    end

    it "tokenizes identity comparison operator" do
      tokens = Volt::Frontend::Lexer.tokenize( "===", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::EqEqEq )
    end

    it "tokenizes match operators" do
      tokens = Volt::Frontend::Lexer.tokenize( "=~ !~", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::MatchOp )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::NotMatchOp )
    end

    it "tokenizes the fat arrow operator" do
      tokens = Volt::Frontend::Lexer.tokenize( "\"a\" => \"b\"", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::String )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::FatArrow )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::String )
    end

    it "distinguishes fat arrow from equals and eq-eq" do
      tokens = Volt::Frontend::Lexer.tokenize( "= == =>", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Eq )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::EqEq )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::FatArrow )
    end

    it "tokenizes the circuit keyword" do
      tokens = Volt::Frontend::Lexer.tokenize( "circuit", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Circuit )
    end

    it "tokenizes bitwise operators" do
      tokens = Volt::Frontend::Lexer.tokenize( "& | ^ ~ << >>", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Amp )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Pipe )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Caret )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Tilde )
      tokens[ 4 ].kind.should eq( Volt::Frontend::TokenKind::LtLt )
      tokens[ 5 ].kind.should eq( Volt::Frontend::TokenKind::GtGt )
    end

    it "tokenizes compound bitwise assignment operators" do
      tokens = Volt::Frontend::Lexer.tokenize( "|= &= ^=", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::PipeEq )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::AmpEq )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::CaretEq )
    end

    it "tokenizes double brace delimiters" do
      tokens = Volt::Frontend::Lexer.tokenize( "{{}}", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::LDoubleBrace )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::RDoubleBrace )
    end

    it "tokenizes macro expression delimiters" do
      tokens = Volt::Frontend::Lexer.tokenize( "{% %}", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::LMacroExpr )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::RMacroExpr )
    end

    it "tokenizes regex literals" do
      tokens = Volt::Frontend::Lexer.tokenize( "/pattern/", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Regex )
    end

    it "tokenizes complex source code" do
      source = <<-VOLT
        class Point
          x : Int64
          y : Int64

          def initialize( @x : Int64, @y : Int64 )
          end

          def distance_to( other : Point ) -> Float64
            dx = @x - other.x
            dy = @y - other.y
            Math.sqrt( dx ** 2 + dy ** 2 )
          end
        end
      VOLT
      tokens = Volt::Frontend::Lexer.tokenize( source, "<test>" )
      tokens.size.should be > 0
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Class )
      tokens.any? { |t| t.kind == Volt::Frontend::TokenKind::Def }.should be_true
      tokens.any? { |t| t.kind == Volt::Frontend::TokenKind::At }.should be_true
      tokens.any? { |t| t.kind == Volt::Frontend::TokenKind::Colon }.should be_true
      tokens.any? { |t| t.kind == Volt::Frontend::TokenKind::StarStar }.should be_true
      tokens.last.kind.should eq( Volt::Frontend::TokenKind::Eof )
    end

    it "tokenizes floor division assignment operator" do
      tokens = Volt::Frontend::Lexer.tokenize( "a //= b", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::SlashSlashEq )
    end

    it "tokenizes compound ampersand assignment operator" do
      tokens = Volt::Frontend::Lexer.tokenize( "&+=", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::AmpPlusEq )
    end

    it "tokenizes all keywords" do
      keywords = "def end class struct mixin component include use module if unless else elsif then when match do while until for break next return and or not in is as async await self super raise abstract true false nil macro __FILE__ __LINE__ __DIR__"
      tokens = Volt::Frontend::Lexer.tokenize( keywords, "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Def )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::End )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Class )
      tokens[ 3 ].kind.should eq( Volt::Frontend::TokenKind::Struct )
      tokens[ 4 ].kind.should eq( Volt::Frontend::TokenKind::Mixin )
      tokens[ 5 ].kind.should eq( Volt::Frontend::TokenKind::Component )
      tokens[ 6 ].kind.should eq( Volt::Frontend::TokenKind::Include )
      tokens[ 7 ].kind.should eq( Volt::Frontend::TokenKind::Use )
      tokens[ 8 ].kind.should eq( Volt::Frontend::TokenKind::Module )
      tokens[ 9 ].kind.should eq( Volt::Frontend::TokenKind::If )
      tokens[ 10 ].kind.should eq( Volt::Frontend::TokenKind::Unless )
      tokens[ 11 ].kind.should eq( Volt::Frontend::TokenKind::Else )
      tokens[ 12 ].kind.should eq( Volt::Frontend::TokenKind::Elsif )
      tokens[ 13 ].kind.should eq( Volt::Frontend::TokenKind::Then )
      tokens[ 14 ].kind.should eq( Volt::Frontend::TokenKind::When )
      tokens[ 15 ].kind.should eq( Volt::Frontend::TokenKind::Match )
      tokens[ 16 ].kind.should eq( Volt::Frontend::TokenKind::Do )
      tokens[ 17 ].kind.should eq( Volt::Frontend::TokenKind::While )
      tokens[ 18 ].kind.should eq( Volt::Frontend::TokenKind::Until )
      tokens[ 19 ].kind.should eq( Volt::Frontend::TokenKind::For )
      tokens[ 20 ].kind.should eq( Volt::Frontend::TokenKind::Break )
      tokens[ 21 ].kind.should eq( Volt::Frontend::TokenKind::Next )
      tokens[ 22 ].kind.should eq( Volt::Frontend::TokenKind::Return )
      tokens[ 23 ].kind.should eq( Volt::Frontend::TokenKind::And )
      tokens[ 24 ].kind.should eq( Volt::Frontend::TokenKind::Or )
      tokens[ 25 ].kind.should eq( Volt::Frontend::TokenKind::Not )
      tokens[ 26 ].kind.should eq( Volt::Frontend::TokenKind::In )
      tokens[ 27 ].kind.should eq( Volt::Frontend::TokenKind::Is )
      tokens[ 28 ].kind.should eq( Volt::Frontend::TokenKind::As )
      tokens[ 29 ].kind.should eq( Volt::Frontend::TokenKind::Async )
      tokens[ 30 ].kind.should eq( Volt::Frontend::TokenKind::Await )
      tokens[ 31 ].kind.should eq( Volt::Frontend::TokenKind::Self_ )
      tokens[ 32 ].kind.should eq( Volt::Frontend::TokenKind::Super )
      tokens[ 33 ].kind.should eq( Volt::Frontend::TokenKind::Raise )
      tokens[ 34 ].kind.should eq( Volt::Frontend::TokenKind::Abstract )
      tokens[ 35 ].kind.should eq( Volt::Frontend::TokenKind::True )
      tokens[ 36 ].kind.should eq( Volt::Frontend::TokenKind::False )
      tokens[ 37 ].kind.should eq( Volt::Frontend::TokenKind::Nil )
      tokens[ 38 ].kind.should eq( Volt::Frontend::TokenKind::Macro )
      tokens[ 39 ].kind.should eq( Volt::Frontend::TokenKind::DunderFile )
      tokens[ 40 ].kind.should eq( Volt::Frontend::TokenKind::DunderLine )
      tokens[ 41 ].kind.should eq( Volt::Frontend::TokenKind::DunderDir )
    end

    it "tokenizes identifiers with special characters" do
      tokens = Volt::Frontend::Lexer.tokenize( "foo? bar! baz_qux", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 0 ].value.should eq( "foo?" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 1 ].value.should eq( "bar!" )
      tokens[ 2 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 2 ].value.should eq( "baz_qux" )
    end

    it "tokenizes invalid characters as errors" do
      tokens = Volt::Frontend::Lexer.tokenize( "\\", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Error )
    end

    it "checks token value equality" do
      tokens = Volt::Frontend::Lexer.tokenize( "foo", "<test>" )
      tokens[ 0 ].value_eq?( "foo" ).should be_true
      tokens[ 0 ].value_eq?( "bar" ).should be_false
      tokens[ 0 ].value_eq?( "foobar" ).should be_false
    end

    it "allows division after appropriate tokens" do
      tokens = Volt::Frontend::Lexer.tokenize( "1 / 2", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )

      tokens = Volt::Frontend::Lexer.tokenize( "1.0 / 2", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )

      tokens = Volt::Frontend::Lexer.tokenize( "\"str\" / 2", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )

      tokens = Volt::Frontend::Lexer.tokenize( "true / 2", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )

      tokens = Volt::Frontend::Lexer.tokenize( "false / 2", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )

      tokens = Volt::Frontend::Lexer.tokenize( "x / 2", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )

      tokens = Volt::Frontend::Lexer.tokenize( "self / 2", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )

      tokens = Volt::Frontend::Lexer.tokenize( "super / 2", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )

      tokens = Volt::Frontend::Lexer.tokenize( ") / 2", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )

      tokens = Volt::Frontend::Lexer.tokenize( "] / 2", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )

      tokens = Volt::Frontend::Lexer.tokenize( "} / 2", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )

      tokens = Volt::Frontend::Lexer.tokenize( "def /", "<test>" )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Slash )
    end

    it "tokenizes all operators and branches" do
      t = Volt::Frontend::Lexer.tokenize( "+ +=", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Plus )
      t[1].kind.should eq( Volt::Frontend::TokenKind::PlusEq )

      t = Volt::Frontend::Lexer.tokenize( "- -> -=", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Minus )
      t[1].kind.should eq( Volt::Frontend::TokenKind::Arrow )
      t[2].kind.should eq( Volt::Frontend::TokenKind::MinusEq )

      t = Volt::Frontend::Lexer.tokenize( "* ** *=", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Star )
      t[1].kind.should eq( Volt::Frontend::TokenKind::StarStar )
      t[2].kind.should eq( Volt::Frontend::TokenKind::StarEq )

      t = Volt::Frontend::Lexer.tokenize( "% %= %}", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Percent )
      t[1].kind.should eq( Volt::Frontend::TokenKind::PercentEq )
      t[2].kind.should eq( Volt::Frontend::TokenKind::RMacroExpr )

      t = Volt::Frontend::Lexer.tokenize( "= == === =~", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Eq )
      t[1].kind.should eq( Volt::Frontend::TokenKind::EqEq )
      t[2].kind.should eq( Volt::Frontend::TokenKind::EqEqEq )
      t[3].kind.should eq( Volt::Frontend::TokenKind::MatchOp )

      t = Volt::Frontend::Lexer.tokenize( "! != !~", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Bang )
      t[1].kind.should eq( Volt::Frontend::TokenKind::BangEq )
      t[2].kind.should eq( Volt::Frontend::TokenKind::NotMatchOp )

      t = Volt::Frontend::Lexer.tokenize( "< << <= <=>", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Lt )
      t[1].kind.should eq( Volt::Frontend::TokenKind::LtLt )
      t[2].kind.should eq( Volt::Frontend::TokenKind::LtEq )
      t[3].kind.should eq( Volt::Frontend::TokenKind::Spaceship )

      t = Volt::Frontend::Lexer.tokenize( "> >> >=", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Gt )
      t[1].kind.should eq( Volt::Frontend::TokenKind::GtGt )
      t[2].kind.should eq( Volt::Frontend::TokenKind::GtEq )

      t = Volt::Frontend::Lexer.tokenize( "& && &+ &+= &- &* &** &=", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Amp )
      t[1].kind.should eq( Volt::Frontend::TokenKind::AmpAmp )
      t[2].kind.should eq( Volt::Frontend::TokenKind::AmpPlus )
      t[3].kind.should eq( Volt::Frontend::TokenKind::AmpPlusEq )
      t[4].kind.should eq( Volt::Frontend::TokenKind::AmpMinus )
      t[5].kind.should eq( Volt::Frontend::TokenKind::AmpStar )
      t[6].kind.should eq( Volt::Frontend::TokenKind::AmpStarStar )
      t[7].kind.should eq( Volt::Frontend::TokenKind::AmpEq )

      t = Volt::Frontend::Lexer.tokenize( "| || |= |>", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Pipe )
      t[1].kind.should eq( Volt::Frontend::TokenKind::PipePipe )
      t[2].kind.should eq( Volt::Frontend::TokenKind::PipeEq )
      t[3].kind.should eq( Volt::Frontend::TokenKind::PipeGt )

      t = Volt::Frontend::Lexer.tokenize( "^ ^=", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Caret )
      t[1].kind.should eq( Volt::Frontend::TokenKind::CaretEq )

      t = Volt::Frontend::Lexer.tokenize( "{ {{ {%", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::LBrace )
      t[1].kind.should eq( Volt::Frontend::TokenKind::LDoubleBrace )
      t[2].kind.should eq( Volt::Frontend::TokenKind::LMacroExpr )

      t = Volt::Frontend::Lexer.tokenize( "} }}", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::RBrace )
      t[1].kind.should eq( Volt::Frontend::TokenKind::RDoubleBrace )
    end

    it "tokenizes operators at end of file" do
      Volt::Frontend::Lexer.tokenize( "+", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Plus )
      Volt::Frontend::Lexer.tokenize( "-", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Minus )
      Volt::Frontend::Lexer.tokenize( "*", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Star )
      Volt::Frontend::Lexer.tokenize( "/", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Error )
      Volt::Frontend::Lexer.tokenize( "%", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Percent )
      Volt::Frontend::Lexer.tokenize( "=", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Eq )
      Volt::Frontend::Lexer.tokenize( "!", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Bang )
      Volt::Frontend::Lexer.tokenize( "<", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Lt )
      Volt::Frontend::Lexer.tokenize( ">", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Gt )
      Volt::Frontend::Lexer.tokenize( "&", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Amp )
      Volt::Frontend::Lexer.tokenize( "|", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Pipe )
      Volt::Frontend::Lexer.tokenize( "^", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Caret )
      Volt::Frontend::Lexer.tokenize( ".", "<test>" )[0].kind.should eq( Volt::Frontend::TokenKind::Dot )
    end

    it "tokenizes dots in all combinations" do
      t = Volt::Frontend::Lexer.tokenize( ".", "<test>" )
      t[0].kind.should eq( Volt::Frontend::TokenKind::Dot )

      t2 = Volt::Frontend::Lexer.tokenize( "..", "<test>" )
      t2[0].kind.should eq( Volt::Frontend::TokenKind::DotDot )

      t3 = Volt::Frontend::Lexer.tokenize( "...", "<test>" )
      t3[0].kind.should eq( Volt::Frontend::TokenKind::DotDotDot )

      t4 = Volt::Frontend::Lexer.tokenize( "..a", "<test>" )
      t4[0].kind.should eq( Volt::Frontend::TokenKind::DotDot )
      t4[1].kind.should eq( Volt::Frontend::TokenKind::Ident )
    end

    it "skips different types of whitespace" do
      tokens = Volt::Frontend::Lexer.tokenize( "a \t\r b", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
    end

    it "handles comment at end of file without newline" do
      tokens = Volt::Frontend::Lexer.tokenize( "a # comment", "<test>" )
      tokens[ 0 ].kind.should eq( Volt::Frontend::TokenKind::Ident )
      tokens[ 1 ].kind.should eq( Volt::Frontend::TokenKind::Eof )
    end

    it "tokenizes number literals comprehensively" do
      t1 = Volt::Frontend::Lexer.tokenize( "123_456 789_u32 111_i64 1_", "<test>" )
      t1[0].kind.should eq( Volt::Frontend::TokenKind::Int )
      t1[1].kind.should eq( Volt::Frontend::TokenKind::Int )
      t1[2].kind.should eq( Volt::Frontend::TokenKind::Int )
      t1[3].kind.should eq( Volt::Frontend::TokenKind::Int )

      t2 = Volt::Frontend::Lexer.tokenize( "3.14_15 1e10 2E-5 3e+4", "<test>" )
      t2[0].kind.should eq( Volt::Frontend::TokenKind::Float )
      t2[1].kind.should eq( Volt::Frontend::TokenKind::Float )
      t2[2].kind.should eq( Volt::Frontend::TokenKind::Float )
      t2[3].kind.should eq( Volt::Frontend::TokenKind::Float )
    end

    it "tokenizes strings with escapes and interpolation" do
      t1 = Volt::Frontend::Lexer.tokenize( "\"hello \\\"world\\\"\"", "<test>" )
      t1[0].kind.should eq( Volt::Frontend::TokenKind::String )

      t2 = Volt::Frontend::Lexer.tokenize( "\"hello #{1 + 2}\"", "<test>" )
      t2[0].kind.should eq( Volt::Frontend::TokenKind::String )

      t3 = Volt::Frontend::Lexer.tokenize( "\"unterminated", "<test>" )
      t3[0].kind.should eq( Volt::Frontend::TokenKind::String )
    end

    it "tokenizes regex with escape sequences and error cases" do
      t1 = Volt::Frontend::Lexer.tokenize( "/abc\\/def/", "<test>" )
      t1[0].kind.should eq( Volt::Frontend::TokenKind::Regex )

      t2 = Volt::Frontend::Lexer.tokenize( "/abc\n", "<test>" )
      t2[0].kind.should eq( Volt::Frontend::TokenKind::Error )

      t3 = Volt::Frontend::Lexer.tokenize( "/abc", "<test>" )
      t3[0].kind.should eq( Volt::Frontend::TokenKind::Error )
    end

  end


end
