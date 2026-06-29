module Volt::Frontend


  class MacroParser

    # Evaluate a `{% if %}` condition. Supports literals, parameter truthiness, and the
    # `==` / `!=` comparisons that cover the overwhelming majority of macro guards.
    private def evaluate_condition( tokens : Array( Token ),
                                    bindings : Hash( String, String ),
                                    call_span : Span ) : Bool
      tokens = strip_newlines( tokens )
      return false if tokens.empty?

      if idx = tokens.index { |t| t.kind.eq_eq? || t.kind.bang_eq? }
        lhs = resolve_atom( tokens[ 0...idx ], bindings, call_span )
        rhs = resolve_atom( tokens[ ( idx + 1 ).. ], bindings, call_span )
        equal = unquote( lhs ) == unquote( rhs )
        return tokens[ idx ].kind.eq_eq? ? equal : !equal
      end

      truthy?( resolve_atom( tokens, bindings, call_span ) )
    end


    private def truthy?( value : String ) : Bool
      v = unquote( value ).strip
      !( v.empty? || v == "false" || v == "nil" )
    end


    # Extract the elements of an array-literal collection: `[a, b, c]`.
    private def collection_items( tokens : Array( Token ),
                                  bindings : Hash( String, String ) ) : Array( String )
      tokens = strip_newlines( tokens )
      raise ExpansionError.new( "`{% for %}` expects an array literal", tokens.first.span ) if tokens.empty?

      unless tokens.first.kind.l_bracket? && tokens.last.kind.r_bracket?
        raise ExpansionError.new( "`{% for %}` expects an array literal", tokens.first.span )
      end

      items   = [] of String
      current = [] of Token
      depth   = 0
      tokens[ 1...-1 ].each do |tok|
        case tok.kind
        when .l_bracket?, .l_paren?, .l_brace?
          depth += 1; current << tok
        when .r_bracket?, .r_paren?, .r_brace?
          depth -= 1; current << tok
        when .comma?
          if depth == 0
            items << reconstruct_with_bindings( current, bindings )
            current = [] of Token
          else
            current << tok
          end
        else
          current << tok
        end
      end
      items << reconstruct_with_bindings( current, bindings ) unless current.empty?
      items
    end

  end


end
