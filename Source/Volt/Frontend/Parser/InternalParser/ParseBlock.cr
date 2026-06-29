module Volt::Frontend


  class Parser

    #------------------------------------------------------------------------------------

    private def parse_brace_block( tok : Token = @current ) : BlockExpr
      loc = tok.span
      advance if tok == @current
      @paren_depth += 1
      params = if @current.kind.pipe?
        parse_block_params
      else
        [] of String
      end
      skip_newlines
      body = parse_body_until( TokenKind::RBrace )
      @paren_depth -= 1
      expect( TokenKind::RBrace )
      BlockExpr.new( params, body, loc )
    end

    private def parse_block_params : Array( String )
      advance   # consume |
      names = [] of String
      skip_newlines
      until @current.kind.pipe? || at_end?
        names << expect( TokenKind::Ident ).value
        skip_newlines
        break unless @current.kind.comma?
        advance; skip_newlines
      end
      expect( TokenKind::Pipe )
      names
    end

    #------------------------------------------------------------------------------------

    private def parse_body_until( terminator : TokenKind ) : Array( ANode )
      nodes = [] of ANode
      skip_separators
      until @current.kind == terminator || at_end?
        nodes << parse_body_node
        skip_separators
      end
      nodes
    end

    #------------------------------------------------------------------------------------

  end


end
