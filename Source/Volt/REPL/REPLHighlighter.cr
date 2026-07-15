require "colorize"

module Volt::REPL


  # Token-level syntax highlighting shared by the result display and the live
  # input line. Context-aware: types, function/method calls, operators and
  # punctuation each get their own face, and lexing errors are shown in red.
  # Only ANSI color codes are added — the visible characters are never changed,
  # which the line editor relies on for cursor math.
  module REPLSyntaxHighlighter

    extend self

    def highlight( source : String ) : String
      tokens = lex( source )
      return source if tokens.nil?

      result = IO::Memory.new
      last_offset = 0_u32

      i = 0
      while i < tokens.size
        tok = tokens[ i ]

        if tok.span.offset > last_offset
          result << source.byte_slice( last_offset, tok.span.offset - last_offset )
        end

        if tok.kind.error?
          # The lexer emits one Error token per unrecognized *byte*, so a single
          # multibyte char arrives shredded: merge the contiguous run and slice
          # the source over the whole range to keep the output valid UTF-8.
          run_start = tok.span.offset
          run_end = run_start + tok.span.length
          while ( following = tokens[ i + 1 ]? ) && following.kind.error? && following.span.offset == run_end
            run_end = following.span.offset + following.span.length
            i += 1
          end
          result << source.byte_slice( run_start, run_end - run_start ).colorize( :red ).underline
          last_offset = run_end
        else
          result << colorize_token( tok, tokens, i )
          last_offset = tok.span.offset + tok.span.length
        end

        i += 1
      end

      if last_offset < source.bytesize
        result << source.byte_slice( last_offset, source.bytesize - last_offset )
      end

      highlighted = result.to_s
      highlighted.valid_encoding? ? highlighted : source
    end

    #------------------------------------------------------------------------------------

    # Collects the token stream, bailing out (nil) on any lexer failure so a
    # half-typed line falls back to plain text instead of garbling the display.
    private def lex( source : String ) : Array(Frontend::Token)?
      lexer = Frontend::Lexer.new source
      tokens = [] of Frontend::Token
      loop do
        tok = lexer.next_token
        break if tok.kind.eof?
        tokens << tok
      end
      tokens
    rescue
      nil
    end

    private def colorize_token( tok : Frontend::Token, tokens : Array(Frontend::Token), index : Int32 ) : String
      val = tok.value

      case tok.kind
      when .int?, .float?
        val.colorize( :magenta ).to_s
      when .string?, .char?, .regex?
        val.colorize( :green ).to_s
      # Enum constant, not `.nil?` — that predicate resolves to `Object#nil?`
      # (always false), which would make this arm dead.
      when .true?, .false?, Frontend::TokenKind::Nil
        val.colorize( :light_cyan ).to_s
      when .newline?
        val
      when .ident?
        colorize_ident( val, tokens, index )
      else
        if keyword?( tok.kind )
          val.colorize( :yellow ).bold.to_s
        elsif operator?( tok.kind )
          val.colorize( :light_red ).to_s
        elsif punctuation?( tok.kind )
          val.colorize( :dark_gray ).to_s
        else
          val.colorize( :light_gray ).to_s
        end
      end
    end

    # Identifiers split three ways: `Types` (capitalized), `calls(...)` or
    # `receiver.method`, and plain variables.
    private def colorize_ident( val : String, tokens : Array(Frontend::Token), index : Int32 ) : String
      if val[ 0 ]?.try( &.uppercase? )
        return val.colorize( :cyan ).to_s
      end

      next_kind = tokens[ index + 1 ]?.try( &.kind )
      prev_kind = index > 0 ? tokens[ index - 1 ].kind : nil

      if next_kind.try( &.l_paren? ) || prev_kind.try( &.dot? ) || prev_kind.try( &.safe_nav? )
        val.colorize( :light_blue ).to_s
      else
        val.colorize( :white ).to_s
      end
    end

    private def keyword?( kind : Frontend::TokenKind ) : Bool
      Frontend::Lexer::KEYWORDS.has_value? kind
    end

    private def operator?( kind : Frontend::TokenKind ) : Bool
      case kind
      when .plus?, .minus?, .star?, .slash?, .percent?, .star_star?,
           .amp_plus?, .amp_minus?, .amp_star?, .amp_star_star?, .slash_slash?,
           .eq_eq?, .bang_eq?, .lt?, .gt?, .lt_eq?, .gt_eq?, .spaceship?, .eq_eq_eq?,
           .match_op?, .not_match_op?, .fat_arrow?, .amp_amp?, .pipe_pipe?, .bang?,
           .eq?, .plus_eq?, .minus_eq?, .star_eq?, .slash_eq?, .slash_slash_eq?,
           .percent_eq?, .pipe_eq?, .amp_eq?, .caret_eq?, .amp_plus_eq?,
           .pipe_gt?, .arrow?, .lt_lt?, .gt_gt?, .amp?, .caret?, .tilde?
        true
      else
        false
      end
    end

    private def punctuation?( kind : Frontend::TokenKind ) : Bool
      case kind
      when .l_paren?, .r_paren?, .l_bracket?, .r_bracket?, .l_brace?, .r_brace?,
           .dot?, .dot_dot?, .dot_dot_dot?, .comma?, .semicolon?, .colon?,
           .colon_colon?, .safe_nav?, .pipe?, .question?
        true
      else
        false
      end
    end

  end


end
