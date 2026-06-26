module Volt::Frontend


  class Parser

    private def parse_grouping : AExpr
      @paren_depth += 1
      expr = parse_expr
      @paren_depth -= 1
      expect( TokenKind::RParen )
      expr
    end

    private def parse_array_lit( tok : Token ) : ArrayLit
      @paren_depth += 1
      elements = [] of AExpr
      skip_newlines
      until @current.kind.r_bracket? || at_end?
        elements << parse_expr
        skip_newlines
        break unless @current.kind.comma?
        advance; skip_newlines
      end
      @paren_depth -= 1
      expect( TokenKind::RBracket )
      ArrayLit.new( elements, tok.span )
    end

  end


end
