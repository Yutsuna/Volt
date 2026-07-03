module Volt::Frontend


  class MacroParser

    # Map each declared parameter name to the verbatim source text of its argument.
    private def bind_parameters( macro_def : MacroDef,
                                  args : Array( Array( Token ) ) ) : Hash( String, String )
      bindings = {} of String => String
      macro_def.params.each_with_index do |param, idx|
        bindings[ param ] = reconstruct( args[ idx ]? || ( [] of Token ) )
      end
      bindings
    end


    # Emit the body tokens in `[from, to)`, honouring `{{ }}` and `{% %}` directives.
    private def emit_sequence( io : String::Builder,
                                body : Array( Token ),
                                from : Int32,
                                to : Int32,
                                bindings : Hash( String, String ),
                                call_span : Span ) : Nil
      i = from
      while i < to
        tok = body[ i ]
        case tok.kind
        when .l_double_brace?
          close = matching( body, i, to, TokenKind::RDoubleBrace, "{{" )
          text  = render_interpolation( body[ ( i + 1 )...close ], bindings, call_span )
          emit_unit( io, text, tok.span.offset, span_end( body[ close ].span ) )
          i = close + 1
        when .l_macro_expr?
          i = emit_directive( io, body, i, to, bindings, call_span )
        when .dunder_file?
          emit_unit( io, quote( call_span.file ), tok.span.offset, span_end( tok.span ) )
          i += 1
        when .dunder_line?
          emit_unit( io, call_span.line.to_s, tok.span.offset, span_end( tok.span ) )
          i += 1
        when .dunder_dir?
          emit_unit( io, quote( directory_of( call_span.file ) ), tok.span.offset, span_end( tok.span ) )
          i += 1
        when .newline?
          io << '\n'
          @cursor = span_end( tok.span )
          i += 1
        else
          emit_unit( io, tok.value, tok.span.offset, span_end( tok.span ) )
          i += 1
        end
      end
    end


    # gree_{{name.id}} -> greet_world
    #
    # Emit one source unit, inserting a single space only when the original source had
    # whitespace before it. Adjacent tokens (no gap) are pasted : this is what makes
    # `greet_{{name.id}}` render as `greet_world`.
    private def emit_unit( io : String::Builder, text : String, start_off : UInt32, end_off : UInt32 ) : Nil
      if ( c = @cursor ) && start_off > c
        io << ' '
      end
      io << text
      @cursor = end_off
    end


    private def span_end( span : Span ) : UInt32
      span.offset + span.length
    end

  end


end
