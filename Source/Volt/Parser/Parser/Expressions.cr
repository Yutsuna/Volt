module Volt
  module Parser

    class FParser

      #--------------------------------------------------------------------------
      # Expressions
      #--------------------------------------------------------------------------

      private def parse_expression : Ast::Expr
        parse_ternary
      end

      private def parse_ternary : Ast::Expr
        cond = parse_binary(0)
        return cond unless accept(Lexer::EToken::Question)
        then_expr = parse_binary(0)
        expect(Lexer::EToken::Colon)
        else_expr = parse_ternary
        Ast::Ternary.new(cond, then_expr, else_expr)
      end

      private def parse_binary ( min_bp : Int32 ) : Ast::Expr
        left = parse_unary
        loop do
          bp = EPrecedence.binding_power(peek.kind)
          break if bp == 0 || bp < min_bp
          op = EPrecedence.symbol(peek.kind)
          advance
          right = parse_binary(bp + 1)
          left = Ast::BinaryOp.new(left, op, right)
        end
        left
      end

      private def parse_unary : Ast::Expr
        case peek.kind
        when Lexer::EToken::Not
          advance ; Ast::UnaryOp.new("!", parse_unary)
        when Lexer::EToken::Tilde
          advance ; Ast::UnaryOp.new("~", parse_unary)
        when Lexer::EToken::Minus
          advance ; Ast::UnaryOp.new("-", parse_unary)
        when Lexer::EToken::KwTypeof
          advance ; Ast::TypeOf.new(parse_unary)
        when Lexer::EToken::KwPointerof
          advance
          expect(Lexer::EToken::LParen)
          operand = parse_expression
          expect(Lexer::EToken::RParen)
          Ast::PointerOf.new(operand)
        else
          parse_primary
        end
      end

      private def parse_primary : Ast::Expr
        expr = parse_primary_base
        while accept(Lexer::EToken::LBracket)
          index_expr = parse_expression
          expect(Lexer::EToken::RBracket)
          expr = Ast::Index.new(expr, index_expr)
        end
        expr
      end

      private def parse_primary_base : Ast::Expr
        t = peek
        case t.kind
        when Lexer::EToken::Integer
          advance ; Ast::IntLit.new(t.int_value, int_type(t.suffix))
        when Lexer::EToken::Float
          advance ; Ast::FloatLit.new(t.float_value, float_type(t.suffix))
        when Lexer::EToken::Char
          advance ; Ast::CharLit.new(t.int_value.to_u8)
        when Lexer::EToken::Str
          advance ; Ast::StrLit.new(t.text)
        when Lexer::EToken::KwTrue
          advance ; Ast::BoolLit.new(true)
        when Lexer::EToken::KwFalse
          advance ; Ast::BoolLit.new(false)
        when Lexer::EToken::KwNil
          advance ; Ast::NilLit.new
        when Lexer::EToken::LParen
          advance
          expr = parse_expression
          expect(Lexer::EToken::RParen)
          expr
        when Lexer::EToken::LBracket
          parse_array
        when Lexer::EToken::Identifier
          parse_identifier_expr
        else
          @reporter.abort!("unexpected token '#{t.lexeme}'", t.line, t.col)
        end
      end

      private def parse_identifier_expr : Ast::Expr
        name = expect(Lexer::EToken::Identifier).lexeme
        if check(Lexer::EToken::LParen)
          Ast::Call.new(name, parse_arg_list)
        elsif command_arg_start?
          Ast::Call.new(name, parse_command_args)
        else
          Ast::VarRef.new(name)
        end
      end

      private def parse_arg_list : Array(Ast::Expr)
        args = [] of Ast::Expr
        expect(Lexer::EToken::LParen)
        unless check(Lexer::EToken::RParen)
          loop do
            args << parse_expression
            break unless accept(Lexer::EToken::Comma)
          end
        end
        expect(Lexer::EToken::RParen)
        args
      end

      private def parse_command_args : Array(Ast::Expr)
        args = [parse_expression]
        while accept(Lexer::EToken::Comma)
          args << parse_expression
        end
        args
      end

      private def parse_array : Ast::ArrayLit
        expect(Lexer::EToken::LBracket)
        elements = [] of Ast::Expr
        skip_newlines
        unless check(Lexer::EToken::RBracket)
          loop do
            elements << parse_expression
            skip_newlines
            break unless accept(Lexer::EToken::Comma)
            skip_newlines
          end
        end
        expect(Lexer::EToken::RBracket)
        Ast::ArrayLit.new(elements)
      end

    end

  end
end
