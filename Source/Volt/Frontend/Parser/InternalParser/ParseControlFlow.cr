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
      parse_expr( Prec::Modifier )
    end

    #------------------------------------------------------------------------------------

    private def strip_quotes( tok : Token ) : String
      raw = tok.value
      raw[ 1, raw.bytesize - 2 ]
    end

    # `"a #{expr} b"` desugars, at parse time, into a left-folded string
    # concatenation `"a " + (expr).to_s + " b"`, reusing the existing `+`
    # (CONCAT_STR) and `.to_s` machinery. A string with no `#{` interpolation
    # is returned as a plain `StringLit`, unchanged.
    private def parse_string_literal( tok : Token ) : AExpr
      content = strip_quotes( tok )
      return StringLit.new( content, tok.span ) unless content.includes?( "\#{" )

      parts   = split_interpolation( content )
      result  = nil.as( AExpr? )
      parts.each do |part|
        piece = case part
                in String   then StringLit.new( part, tok.span ).as( AExpr )
                in AExpr     then MemberAccess.new( part, "to_s", false, tok.span ).as( AExpr )
                end
        result = result.nil? ? piece : BinaryOp.new( result, TokenKind::Plus, piece, tok.span )
      end
      result || StringLit.new( "", tok.span )
    end

    # Splits raw string content into a sequence of literal `String` segments and
    # parsed `AExpr` interpolations (each `#{ ... }`), preserving order. Brace
    # nesting inside an interpolation is tracked so `#{ f( g ) }` closes on its
    # matching `}`.
    private def split_interpolation( content : String ) : Array( String | AExpr )
      segments = [] of ( String | AExpr )
      literal  = String::Builder.new
      i        = 0
      bytes    = content
      while i < bytes.size
        if bytes[ i ] == '#' && i + 1 < bytes.size && bytes[ i + 1 ] == '{'
          unless literal.empty?
            segments << literal.to_s
            literal = String::Builder.new
          end
          depth = 1
          i    += 2
          start = i
          while i < bytes.size && depth > 0
            case bytes[ i ]
            when '{' then depth += 1
            when '}' then depth -= 1
            end
            i += 1 if depth > 0
          end
          expr_src = bytes[ start, i - start ]
          segments << parse_interpolated_expr( expr_src )
          i += 1   # consume the closing `}`
        else
          literal << bytes[ i ]
          i += 1
        end
      end
      segments << literal.to_s unless literal.empty?
      segments
    end

    # Parses a single interpolation fragment (the text between `#{` and `}`)
    # into an expression via a throwaway sub-parser, forwarding any diagnostics
    # it raises back into this parser's bag.
    private def parse_interpolated_expr( source : String ) : AExpr
      sub  = Parser.new( source, @file )
      expr = sub.parse_expansion_expr
      sub.bag.each { |d| @bag << d } if sub.bag.errors?
      expr
    end

  end


end
