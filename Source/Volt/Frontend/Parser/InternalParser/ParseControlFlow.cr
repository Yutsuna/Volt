module Volt::Frontend


  class Parser

    #------------------------------------------------------------------------------------

    private def parse_if_expr( tok : Token ) : IfExpr
      cond   = parse_expr( Prec::None )
      skip_separators
      then_b = parse_body
      elsifs = [] of { AExpr, Array( ANode ) }
      else_b = nil

      while @current.kind.elsif?
        advance
        ec = parse_expr( Prec::None )
        skip_separators
        eb = parse_body
        elsifs << { ec, eb }
      end

      if @current.kind.else?
        advance
        skip_separators
        else_b = parse_body
      end

      expect( TokenKind::End )
      IfExpr.new( cond, then_b, elsifs, else_b, tok.span )
    end

    #------------------------------------------------------------------------------------

    private def parse_unless_expr( tok : Token ) : IfExpr
      cond   = parse_expr( Prec::None )
      skip_separators
      body   = parse_body
      else_b = nil
      if @current.kind.else?
        advance; skip_separators
        else_b = parse_body
      end
      expect( TokenKind::End )
      neg = UnaryOp.new( TokenKind::Bang, cond, tok.span )
      IfExpr.new( neg, body, [] of { AExpr, Array( ANode ) }, else_b, tok.span )
    end

    #------------------------------------------------------------------------------------

    private def parse_while_expr( tok : Token ) : WhileExpr
      cond = parse_expr( Prec::None )
      skip_separators
      body = parse_body
      expect( TokenKind::End )
      WhileExpr.new( cond, body, tok.span )
    end

    #------------------------------------------------------------------------------------

    private def parse_until_expr( tok : Token ) : WhileExpr
      cond = parse_expr( Prec::None )
      skip_separators
      body = parse_body
      expect( TokenKind::End )
      neg = UnaryOp.new( TokenKind::Bang, cond, tok.span )
      WhileExpr.new( neg, body, tok.span )
    end

    #------------------------------------------------------------------------------------

    private def parse_match_expr( tok : Token ) : MatchExpr
      value = parse_expr( Prec::None )
      skip_separators
      arms  = [] of MatchArm

      while @current.kind.when? || @current.kind.else?
        if @current.kind.else?
          advance; skip_separators
          body = parse_expr( Prec::None )
          arms << MatchArm.new( [] of AExpr, body, true )
          break
        end
        advance   # consume `when`
        patterns = [] of AExpr
        patterns << parse_expr( Prec::Range )
        while @current.kind.comma?
          advance; patterns << parse_expr( Prec::Range )
        end
        skip_newlines
        advance if @current.kind.then?   # optional `then`
        skip_newlines
        body = parse_expr( Prec::None )
        arms << MatchArm.new( patterns, body, false )
        skip_separators
      end

      expect( TokenKind::End )
      MatchExpr.new( value, arms, tok.span )
    end

    #------------------------------------------------------------------------------------

    private def expr_if_on_same_line : AExpr?
      return nil if @current.kind.newline? || @current.kind.semicolon? || at_end?
      parse_expr( Prec::None )
    end

    #------------------------------------------------------------------------------------

    private def strip_quotes( tok : Token ) : String
      raw = tok.value
      raw[ 1, raw.bytesize - 2 ]
    end

  end


end
