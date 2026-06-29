module Volt::Frontend


  class MacroParser

    # Render the contents of a `{{ }}` interpolation. The general shape we support is
    # `<atom>` or `<atom>.<method>`, where `<atom>` is a macro parameter, a pseudo-var
    # or a literal. Anything else is passed through verbatim.
    private def render_interpolation( inner : Array( Token ),
                                      bindings : Hash( String, String ),
                                      call_span : Span ) : String
      return "" if inner.empty?

      # Split a single trailing `.method` (no arguments) off the atom.
      if inner.size >= 3 && inner[ -2 ].kind.dot? && inner[ -1 ].kind.ident?
        atom   = resolve_atom( inner[ 0...-2 ], bindings, call_span )
        method = inner[ -1 ].value
        return apply_method( atom, method, inner[ -1 ].span )
      end

      resolve_atom( inner, bindings, call_span )
    end


    # Resolve the textual value an interpolation atom denotes.
    private def resolve_atom( tokens : Array( Token ),
                              bindings : Hash( String, String ),
                              call_span : Span ) : String
      if tokens.size == 1
        tok = tokens.first
        case tok.kind
        when .ident?       then return bindings[ tok.value ]? || tok.value
        when .dunder_file? then return quote( call_span.file )
        when .dunder_line? then return call_span.line.to_s
        when .dunder_dir?  then return quote( directory_of( call_span.file ) )
        end
      end

      # Compound expression: substitute parameter idents, pass the rest through.
      String.build do |io|
        tokens.each_with_index do |tok, idx|
          io << ' ' if idx > 0
          if tok.kind.ident? && ( val = bindings[ tok.value ]? )
            io << val
          else
            io << tok.value
          end
        end
      end
    end


    # Compile-time string methods available inside `{{ }}`.
    private def apply_method( value : String, method : String, span : Span ) : String
      text = unquote( value )
      case method
      when "id"        then text
      when "stringify" then quote( text )
      when "upcase"    then quote( text.upcase )
      when "downcase"  then quote( text.downcase )
      when "camelcase" then quote( text.split( /[_\s]+/ ).map( &.capitalize ).join )
      when "empty?"    then text.empty?.to_s
      when "size"      then text.size.to_s
      else
        raise ExpansionError.new( "unknown macro method `#{ method }`", span )
      end
    end

  end


end
