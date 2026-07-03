require "colorize"

module Volt::REPL


  module REPLSyntaxHighlighter

    extend self

    def highlight( source : String ) : String
      lexer = Frontend::Lexer.new source
      result = IO::Memory.new
      last_offset = 0_u32

      loop do
        tok = lexer.next_token
        break if tok.kind.eof?

        if tok.span.offset > last_offset
          result << source[ last_offset...tok.span.offset ]
        end

        val = tok.value
        case tok.kind
        when .int?, .float?
          result << val.colorize( :magenta )
        when .string?, .regex?
          result << val.colorize( :green )
        when .true?, .false?, .nil?
          result << val.colorize( :cyan )
        when .ident?
          result << val.colorize( :white )
        when .newline?
          result << val
        else
          if keyword?(tok.kind)
            result << val.colorize( :yellow ).bold
          else
            result << val.colorize( :light_gray )
          end
        end

        last_offset = tok.span.offset + tok.span.length
      end

      if last_offset < source.bytesize
        result << source[ last_offset..-1 ]
      end

      result.to_s
    end

    private def keyword?( kind : Frontend::TokenKind ) : Bool
      Frontend::Lexer::KEYWORDS.has_value? kind
    end

  end


end
