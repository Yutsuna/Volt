module Volt
  module Parser


    class FParser

      #--------------------------------------------------------------------------
      # Statements
      #--------------------------------------------------------------------------

      private def parse_block ( terminators : Array(Lexer::EToken) ) : Array(Ast::Node)
        stmts = [] of Ast::Node
        skip_newlines
        until check(Lexer::EToken::EOF) || terminators.any? { |t| check(t) }
          stmts << parse_statement
          skip_newlines
        end
        stmts
      end

      private def parse_statement : Ast::Node
        case peek.kind
        when Lexer::EToken::KwIf     then parse_if
        when Lexer::EToken::KwUnless then parse_unless
        when Lexer::EToken::KwWhile  then parse_while(false)
        when Lexer::EToken::KwUntil  then parse_while(true)
        else                              parse_simple_statement
        end
      end

      private def parse_simple_statement : Ast::Node
        stmt =
          if check(Lexer::EToken::KwReturn)
            parse_return
          elsif assignment_ahead?
            parse_assignment
          else
            Ast::ExprStmt.new(parse_expression)
          end
        apply_modifier(stmt)
      end

      private def apply_modifier ( stmt : Ast::Node ) : Ast::Node
        if accept(Lexer::EToken::KwIf)
          Ast::If.new(parse_expression, [stmt])
        elsif accept(Lexer::EToken::KwUnless)
          Ast::If.new(negate(parse_expression), [stmt])
        else
          stmt
        end
      end

      private def parse_return : Ast::Return
        expect(Lexer::EToken::KwReturn)
        if expression_ahead?
          Ast::Return.new(parse_expression)
        else
          Ast::Return.new
        end
      end

      private def parse_if : Ast::If
        expect(Lexer::EToken::KwIf)
        cond = parse_expression
        then_body = parse_block([Lexer::EToken::KwElsif, Lexer::EToken::KwElse, Lexer::EToken::KwEnd])
        else_body = parse_if_tail
        expect(Lexer::EToken::KwEnd)
        Ast::If.new(cond, then_body, else_body)
      end

      private def parse_if_tail : Array(Ast::Node)?
        if check(Lexer::EToken::KwElsif)
          expect(Lexer::EToken::KwElsif)
          cond = parse_expression
          body = parse_block([Lexer::EToken::KwElsif, Lexer::EToken::KwElse, Lexer::EToken::KwEnd])
          [Ast::If.new(cond, body, parse_if_tail).as(Ast::Node)]
        elsif accept(Lexer::EToken::KwElse)
          parse_block([Lexer::EToken::KwEnd])
        else
          nil
        end
      end

      private def parse_unless : Ast::If
        expect(Lexer::EToken::KwUnless)
        cond = parse_expression
        body = parse_block([Lexer::EToken::KwElse, Lexer::EToken::KwEnd])
        else_body = accept(Lexer::EToken::KwElse) ? parse_block([Lexer::EToken::KwEnd]) : nil
        expect(Lexer::EToken::KwEnd)
        Ast::If.new(negate(cond), body, else_body)
      end

      private def parse_while ( is_until : Bool ) : Ast::While
        advance # while / until
        cond = parse_expression
        accept(Lexer::EToken::KwDo)
        body = parse_block([Lexer::EToken::KwEnd])
        expect(Lexer::EToken::KwEnd)
        Ast::While.new(cond, body, is_until)
      end

      private def parse_assignment : Ast::Assign
        name = expect(Lexer::EToken::Identifier).lexeme
        declared = nil
        if accept(Lexer::EToken::Colon)
          declared = parse_type
          expect(Lexer::EToken::Assign)
          value = parse_expression
        elsif accept(Lexer::EToken::Assign)
          value = parse_expression
        elsif accept(Lexer::EToken::PlusAssign)
          value = Ast::BinaryOp.new(Ast::VarRef.new(name), "+", parse_expression)
        elsif accept(Lexer::EToken::MinusAssign)
          value = Ast::BinaryOp.new(Ast::VarRef.new(name), "-", parse_expression)
        else
          @reporter.abort!("expected assignment operator", peek.line, peek.col)
        end
        Ast::Assign.new(name, value, declared)
      end

      private def assignment_ahead? : Bool
        return false unless check(Lexer::EToken::Identifier)
        case peek(1).kind
        when Lexer::EToken::Colon, Lexer::EToken::Assign,
             Lexer::EToken::PlusAssign, Lexer::EToken::MinusAssign
          true
        else
          false
        end
      end

      #--------------------------------------------------------------------------

    end


  end
end
