require "./Parser/TopLevels"
require "./Parser/Statements"
require "./Parser/Expressions"

module Volt
  module Parser


    class FParser

      SUFFIX_TYPES = {
        "u8"  => Types::EType::UInt8,
        "u16" => Types::EType::UInt16,
        "u32" => Types::EType::UInt32,
        "u64" => Types::EType::UInt64,
        "i8"  => Types::EType::Int8,
        "i16" => Types::EType::Int16,
        "i32" => Types::EType::Int32,
        "i64" => Types::EType::Int64,
        "f32" => Types::EType::Float32,
        "f64" => Types::EType::Float64,
      }

      ARG_STARTERS = [
        Lexer::EToken::Integer, Lexer::EToken::Float, Lexer::EToken::Char,
        Lexer::EToken::Str, Lexer::EToken::Identifier, Lexer::EToken::KwTrue,
        Lexer::EToken::KwFalse, Lexer::EToken::KwNil, Lexer::EToken::KwTypeof,
        Lexer::EToken::KwPointerof,
      ]

      EXPR_STARTERS = ARG_STARTERS + [
        Lexer::EToken::Minus, Lexer::EToken::Not, Lexer::EToken::Tilde,
        Lexer::EToken::LParen, Lexer::EToken::LBracket,
      ]

      #--------------------------------------------------------------------------

      def initialize ( @tokens : Array(Lexer::Token), @reporter : Diagnostic::FReporter )
        @pos = 0
      end

      #--------------------------------------------------------------------------

      def parse : Ast::Program
        program = Ast::Program.new
        skip_newlines
        until check(Lexer::EToken::EOF)
          if check(Lexer::EToken::AnnotationStart)
            program.externs << parse_extern
          elsif check(Lexer::EToken::KwDef)
            program.defs << parse_def
          else
            program.top_level << parse_statement
          end
          skip_newlines
        end
        program
      end

      #--------------------------------------------------------------------------
      # Helpers
      #--------------------------------------------------------------------------

      private def int_type ( suffix : String ) : Types::Type
        base = SUFFIX_TYPES[suffix]? || Types::EType::Int32
        Types::Type.new(base)
      end

      private def float_type ( suffix : String ) : Types::Type
        base = SUFFIX_TYPES[suffix]? || Types::EType::Float64
        Types::Type.new(base)
      end

      private def negate ( expr : Ast::Expr ) : Ast::Expr
        Ast::UnaryOp.new("!", expr)
      end

      private def command_arg_start? : Bool
        ARG_STARTERS.includes?(peek.kind)
      end

      private def expression_ahead? : Bool
        EXPR_STARTERS.includes?(peek.kind)
      end

      private def skip_newlines : Nil
        while check(Lexer::EToken::Newline)
          advance
        end
      end

      private def peek ( offset : Int32 = 0 ) : Lexer::Token
        i = @pos + offset
        i < @tokens.size ? @tokens[i] : @tokens[-1]
      end

      private def check ( kind : Lexer::EToken ) : Bool
        peek.kind == kind
      end

      private def advance : Lexer::Token
        t = peek
        @pos += 1 if @pos < @tokens.size - 1
        t
      end

      private def accept ( kind : Lexer::EToken ) : Bool
        return false unless check(kind)
        advance
        true
      end

      private def expect ( kind : Lexer::EToken ) : Lexer::Token
        return advance if check(kind)
        @reporter.abort!("expected #{kind}, got '#{peek.lexeme}' (#{peek.kind})", peek.line, peek.col)
      end

    end


  end
end
