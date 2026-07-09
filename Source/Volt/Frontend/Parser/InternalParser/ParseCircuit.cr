module Volt::Frontend


  class Parser

    # circuit "Name" { runtime "..." ; entrypoint "..." ; modules( "A" => "dir", ... ) }
    # The `do ... end` form is also accepted (Ruby/Crystal block equivalence).
    private def parse_circuit_decl : CircuitDecl
      loc = @current.span
      advance   # consume `circuit`
      name = expect_plain_string
      skip_newlines

      closer = expect_circuit_block_open
      skip_separators

      runtime    = nil.as( String? )
      entrypoint = nil.as( String? )
      modules    = nil.as( HashLiteralExpr? )

      until @current.kind == closer || at_end?
        unless @current.kind.ident?
          error!( Catalog::Parse.expected_one_of( "a circuit entry (`runtime`, `entrypoint`, or `modules`)", @current ) )
        end

        case @current.value
        when "runtime"
          advance
          runtime = parse_maybe_parenthesized_string
        when "entrypoint"
          advance
          entrypoint = parse_maybe_parenthesized_string
        when "modules"
          advance
          modules = parse_circuit_modules
        else
          error!( Catalog::Parse.expected_one_of( "a circuit entry (`runtime`, `entrypoint`, or `modules`)", @current ) )
        end
        skip_separators
      end

      expect_close( closer, loc, "`circuit`" )
      CircuitDecl.new( name, runtime, entrypoint, modules, loc )
    end

    # Consumes the opening delimiter of a circuit block and reports which
    # delimiter must close it (`{` -> `}`, `do` -> `end`).
    private def expect_circuit_block_open : TokenKind
      case @current.kind
      when .l_brace?
        advance
        TokenKind::RBrace
      when .do?
        advance
        TokenKind::End
      else
        error!( Catalog::Parse.expected_after( "`{` or `do`", "circuit name", @current ) )
      end
    end

    # "A" => "dir/path" , ...
    private def parse_circuit_modules : HashLiteralExpr
      loc = @current.span
      expect( TokenKind::LParen )
      @paren_depth += 1
      pairs = [] of { AExpr, AExpr }
      skip_newlines
      until @current.kind.r_paren? || at_end?
        key_tok = expect( TokenKind::String )
        key     = StringLit.new( expect_plain_string_value( key_tok ), key_tok.span ).as( AExpr )
        skip_newlines
        expect( TokenKind::FatArrow )
        skip_newlines
        value_tok = expect( TokenKind::String )
        value     = StringLit.new( expect_plain_string_value( value_tok ), value_tok.span ).as( AExpr )
        pairs << { key, value }
        skip_newlines
        break unless @current.kind.comma?
        advance; skip_newlines
      end
      @paren_depth -= 1
      expect( TokenKind::RParen )
      HashLiteralExpr.new( pairs, loc )
    end

    # Consumes a `TokenKind::String` token and returns its plain (unescaped,
    # non-interpolated) contents. Circuit manifests are static data: `#{...}`
    # interpolation would make dependency resolution depend on runtime values,
    # so it is rejected here rather than silently desugared.
    private def expect_plain_string : String
      expect_plain_string_value( expect( TokenKind::String ) )
    end

    private def parse_maybe_parenthesized_string : String
      if @current.kind.l_paren?
        advance
        @paren_depth += 1
        val = expect_plain_string
        @paren_depth -= 1
        expect( TokenKind::RParen )
        val
      else
        expect_plain_string
      end
    end

    private def expect_plain_string_value( tok : Token ) : String
      content = strip_quotes( tok )
      if content.includes?( "\#{" )
        error!( Catalog::Parse.circuit_no_interpolation( tok.span ) )
      end
      unescape( content )
    end

  end


end
