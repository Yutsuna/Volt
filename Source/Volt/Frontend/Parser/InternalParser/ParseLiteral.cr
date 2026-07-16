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
      ArrayLit.new( elements, tok.span, parse_of_annotation )
    end

    #------------------------------------------------------------------------------------

    # `{ "a" => 1, :b => 2, c: 3 }` | `{} of K => V` — reached from `nud` for
    # any `{` in expression position that doesn't open a parameterised block
    # (`{ |x| ... }`). Two key spellings per pair : `expr => value`, and the
    # `name: value` shorthand for a symbol key.
    private def parse_hash_literal( tok : Token ) : HashLiteralExpr
      @paren_depth += 1
      pairs = [] of { AExpr, AExpr }
      skip_newlines
      until @current.kind.r_brace? || at_end?
        pairs << parse_hash_pair
        skip_newlines
        break unless @current.kind.comma?
        advance; skip_newlines
      end
      @paren_depth -= 1
      expect( TokenKind::RBrace )
      key_ann, val_ann = parse_hash_of_annotation
      HashLiteralExpr.new( pairs, tok.span, key_ann, val_ann )
    end

    private def parse_hash_pair : { AExpr, AExpr }
      # `name: value` — symbol-key shorthand. Only the exact Ident+`:` token
      # pair triggers it, so a general expression key never starts this path.
      if @current.kind.ident? && @peek.kind.colon?
        key_tok = advance
        advance   # consume `:`
        skip_newlines
        Symbols.intern( key_tok.value )
        return { SymbolLit.new( key_tok.value, key_tok.span ).as( AExpr ),
                 parse_expr( Prec::Pipe ) }
      end
      key = parse_expr( Prec::Pipe )
      expect( TokenKind::FatArrow )
      skip_newlines
      { key, parse_expr( Prec::Pipe ) }
    end

    #------------------------------------------------------------------------------------

    # Contextual `of` — deliberately *not* a keyword (an identifier named
    # `of` anywhere else stays a plain `Ident`) : it only means "element
    # type annotation" immediately after a `[...]` / `{...}` literal, on the
    # same line.
    private def of_follows? : Bool
      @current.kind.ident? && @current.value == "of"
    end

    # `[ 1, 2 ] of UInt8` : the element type of an array literal.
    private def parse_of_annotation : ATypeNode?
      return nil unless of_follows?
      advance   # consume `of`
      parse_type
    end

    # `{} of String => Int32` : the key/value types of a hash literal.
    private def parse_hash_of_annotation : { ATypeNode?, ATypeNode? }
      return { nil, nil } unless of_follows?
      advance   # consume `of`
      key_ann = parse_type
      expect( TokenKind::FatArrow )
      { key_ann, parse_type }
    end

  end


end
