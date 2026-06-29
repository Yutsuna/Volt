module Volt::Frontend


  class Parser

    def parse_expr( min_prec : Prec = Prec::None ) : AExpr
      skip_newlines if @paren_depth > 0
      left = nud( advance )
      loop do
        skip_newlines if @paren_depth > 0

        if @paren_depth == 0 && @current.kind.newline?
          continuation = led_prec( @peek.kind )
          break if continuation <= min_prec
          advance
        end

        if min_prec < Prec::Call && ( left.is_a?( Ident ) || left.is_a?( MethodCall ) ) && can_start_expr?( @current.kind )
          if led_prec( @current.kind ) == Prec::None
            args = parse_space_call_args
            blk = if @current.kind.l_brace?
              parse_brace_block
            end

            left = if left.is_a?( Ident )
              Call.new( left, args, blk, left.loc )
            else # MethodCall
              left.args.concat( args )
              left.block = blk if blk
              left
            end
            next
          end
        end

        bp = led_prec( @current.kind )
        break if bp <= min_prec
        op   = advance
        left = led( left, op )
      end
      left
    end

    private def nud( tok : Token ) : AExpr
      case tok.kind
      when .int?
        val_str = tok.value
        suffix = nil
        if idx = val_str.index( /_[iu]/ )
          suffix = val_str[ idx..-1 ]
          val_str = val_str[ 0...idx ]
        end
        clean_str = val_str.delete( '_' )
        node = IntLit.new( clean_str.to_i64, tok.span )
        node.resolved_type = resolve_suffix_type( suffix )
        node
      when .float?
        val_str = tok.value
        clean_str = val_str.delete( '_' )
        node = FloatLit.new( clean_str.to_f64, tok.span )
        node.resolved_type = Type::FLOAT
        node
      when .string?
        StringLit.new( strip_quotes( tok ), tok.span )
      when .true?
        BoolLit.new( true, tok.span )
      when .false?
        BoolLit.new( false, tok.span )
      when .nil?
        NilLit.new( tok.span )
      when .ident?
        Ident.new( tok.value, tok.span )
      when .self_?
        SelfExpr.new( tok.span )
      when .l_paren?
        parse_grouping
      when .l_bracket?
        parse_array_lit( tok )
      when .l_brace?
        parse_brace_block( tok )
      when .minus?
        operand = parse_expr( Prec::Unary )
        UnaryOp.new( TokenKind::Minus, operand, tok.span )
      when .plus?
        parse_expr( Prec::Unary )
      when .tilde?
        operand = parse_expr( Prec::Unary )
        UnaryOp.new( TokenKind::Tilde, operand, tok.span )
      when .dunder_file?
        StringLit.new( @file, tok.span )
      when .dunder_line?
        IntLit.new( tok.span.line.to_i64, tok.span )
      when .regex?
        raw = tok.value
        pattern = raw[ 1, raw.bytesize - 2 ]
        RegexLit.new( pattern, tok.span )
      when .bang?, .not?
        operand = parse_expr( Prec::Unary )
        UnaryOp.new( TokenKind::Bang, operand, tok.span )
      when .if?
        parse_if_expr( tok )
      when .unless?
        parse_unless_expr( tok )
      when .match?
        parse_match_expr( tok )
      when .while?
        parse_while_expr( tok )
      when .until?
        parse_until_expr( tok )
      when .await?
        AwaitExpr.new( parse_expr( Prec::Unary ), tok.span )
      when .return?
        ReturnExpr.new( expr_if_on_same_line, tok.span )
      when .break?
        BreakExpr.new( expr_if_on_same_line, tok.span )
      when .raise?
        RaiseExpr.new( parse_expr( Prec::Unary ), tok.span )
      else
        error!( Catalog::Parse.unexpected_expr( tok ) )
      end
    end

    private def led( left : AExpr, op : Token ) : AExpr
      case op.kind
      when .plus?, .minus?, .star?, .slash?, .percent?,
           .amp_plus?, .amp_minus?, .amp_star?, .slash_slash?,
           .amp?, .pipe?, .caret?, .lt_lt?, .gt_gt?,
           .spaceship?, .eq_eq_eq?, .match_op?, .not_match_op?,
           .eq_eq?, .bang_eq?, .lt?, .gt?, .lt_eq?, .gt_eq?
        BinaryOp.new( left, op.kind, parse_expr( led_prec( op.kind ) ), left.loc )
      when .star_star?, .amp_star_star?
        BinaryOp.new( left, op.kind, parse_expr( Prec.new( Prec::Power.value - 1 ) ), left.loc )
      when .plus_eq?, .minus_eq?, .star_eq?, .slash_eq?, .slash_slash_eq?,
           .percent_eq?, .pipe_eq?, .amp_eq?, .caret_eq?, .amp_plus_eq?
        bin_op = resolve_binary_operator( op.kind )
        rhs = parse_expr( Prec.new( Prec::Assignment.value - 1 ) )
        val = BinaryOp.new( left, bin_op, rhs, left.loc )
        Assign.new( left, nil, val, left.loc )
      when .and?, .amp_amp?
        BinaryOp.new( left, TokenKind::And, parse_expr( Prec::And ), left.loc )
      when .or?, .pipe_pipe?
        BinaryOp.new( left, TokenKind::Or, parse_expr( Prec::Or ), left.loc )
      when .dot_dot?
        RangeExpr.new( left, parse_expr( led_prec( op.kind ) ), false, left.loc )
      when .dot_dot_dot?
        RangeExpr.new( left, parse_expr( led_prec( op.kind ) ), true, left.loc )
      when .pipe_gt?
        PipeExpr.new( left, parse_expr( Prec::Pipe ), left.loc )
      when .dot?
        parse_dot_call( left, safe: false )
      when .question?
        expect( TokenKind::Dot )
        parse_dot_call( left, safe: true )
      when .l_paren?
        if left.is_a?( Ident ) && ( macro_def = @macro_table[ left.name ]? )
          return expand_macro( macro_def, op )
        end
        parse_call_with_open_paren( left, op )
      when .l_brace?
        blk = parse_brace_block( op )
        case left
        when Call
          Call.new( left.callee, left.args, blk, left.loc )
        when MethodCall
          MethodCall.new( left.receiver, left.name, left.args, blk, left.safe, left.loc )
        else
          Call.new( left, [] of AExpr, blk, left.loc )
        end
      when .l_bracket?
        idx = parse_expr( Prec::None )
        expect( TokenKind::RBracket )
        Index.new( left, idx, left.loc )
      when .eq?
        rhs = parse_expr( Prec.new( Prec::Assignment.value - 1 ) )
        Assign.new( left, nil, rhs, left.loc )
      when .colon?
        ty  = parse_type
        expect( TokenKind::Eq )
        rhs = parse_expr( Prec::None )
        Assign.new( left, ty, rhs, left.loc )
      else
        error!( Catalog::Parse.unexpected_infix( op ) )
      end
    end

    private def led_prec( kind : TokenKind ) : Prec
      case kind
      when .eq?, .colon?, .plus_eq?, .minus_eq?, .star_eq?, .slash_eq?, .slash_slash_eq?,
           .percent_eq?, .pipe_eq?, .amp_eq?, .caret_eq?, .amp_plus_eq?
        Prec::Assignment
      when .or?, .pipe_pipe?                     then Prec::Or
      when .and?, .amp_amp?                      then Prec::And
      when .eq_eq?, .bang_eq?, .eq_eq_eq?,
           .match_op?, .not_match_op?            then Prec::Equality
      when .lt?, .gt?, .lt_eq?, .gt_eq?,
           .spaceship?                           then Prec::Comparison
      when .dot_dot?, .dot_dot_dot?              then Prec::Range
      when .pipe_gt?                             then Prec::Pipe
      when .pipe?, .caret?                       then Prec::BitOr
      when .amp?                                 then Prec::BitAnd
      when .lt_lt?, .gt_gt?                      then Prec::Shift
      when .plus?, .minus?, .amp_plus?,
           .amp_minus?                           then Prec::Term
      when .star?, .slash?, .percent?,
           .slash_slash?, .amp_star?             then Prec::Factor
      when .star_star?, .amp_star_star?          then Prec::Power
      when .dot?, .question?                     then Prec::Call
      when .l_paren?, .l_bracket?, .l_brace?    then Prec::Call
      else                                        Prec::None
      end
    end

    private def resolve_suffix_type( suffix : String? ) : Type
      case suffix
      when "_i8"  then Type::INT8
      when "_i16" then Type::INT16
      when "_i32" then Type::INT32
      else             Type::INT
      end
    end

    private def resolve_binary_operator( kind : TokenKind ) : TokenKind
      case op.kind
      when .plus_eq?        then TokenKind::Plus
      when .minus_eq?       then TokenKind::Minus
      when .star_eq?        then TokenKind::Star
      when .slash_eq?       then TokenKind::Slash
      when .slash_slash_eq? then TokenKind::SlashSlash
      when .percent_eq?     then TokenKind::Percent
      when .pipe_eq?        then TokenKind::Pipe
      when .amp_eq?         then TokenKind::Amp
      when .caret_eq?       then TokenKind::Caret
      when .amp_plus_eq?    then TokenKind::AmpPlus
      else                       raise "unreachable"
      end
    end

    private def collect_macro_args : Array( Array( Token ) )
      args = [] of Array( Token )
      current_arg = [] of Token
      depth = 1

      until at_end?
        tok = @current
        if tok.kind.l_paren? || tok.kind.l_bracket? || tok.kind.l_brace?
          depth += 1
          current_arg << advance
        elsif tok.kind.r_paren? || tok.kind.r_bracket? || tok.kind.r_brace?
          depth -= 1
          if depth == 0
            advance   # consume closing RParen
            args << current_arg unless current_arg.empty? && args.empty?
            break
          else
            current_arg << advance
          end
        elsif tok.kind.comma? && depth == 1
          advance   # consume comma
          args << current_arg
          current_arg = [] of Token
        else
          current_arg << advance
        end
      end
      args
    end

    private def macro_invocation? : Bool
      @current.kind.ident? && @macro_table.has_key?( @current.value )
    end

    private def expand_macro_statements : Array( ANode )
      call_span = @current.span
      macro_def = @macro_table[ @current.value ]
      advance   # consume the macro name
      args = collect_macro_call_args
      parse_macro_expansion( macro_def, call_span, args ) do |sub|
        sub.parse_expansion_nodes
      end
    end

    # Expand a macro used in expression position (`x = some_macro(...)`) into a single
    # expression, re-parsing the rendered source.
    private def expand_macro( macro_def : MacroDef, open_paren : Token ) : AExpr
      args = collect_macro_args
      parse_macro_expansion( macro_def, open_paren.span, args ) do |sub|
        sub.parse_expansion_expr
      end
    end

    private def parse_macro_expansion( macro_def : MacroDef, call_span : Span,
                                       args : Array( Array( Token ) ), & : Parser -> T ) : T forall T
      if @macro_depth >= MAX_MACRO_DEPTH
        error!( Catalog::Parse.macro_expansion(
          "maximum expansion depth (#{ MAX_MACRO_DEPTH }) exceeded — `#{ macro_def.name }` is likely recursive",
          call_span ) )
      end

      source = begin
                 macro_expander.expand( macro_def, args, call_span )
               rescue ex : MacroExpander::ExpansionError
                 error!( Catalog::Parse.macro_expansion( ex.message || "invalid macro", ex.span ) )
               end
      tokens = Lexer.tokenize( source, @file )
      sub    = Parser.new( tokens, @file, @bag )
      sub.import_macros( @macro_table, @macro_depth + 1 )
      yield sub
    end

    # Collect the argument token groups of a macro call, supporting the parenthesised
    # `m(a, b)`, the space-separated `m a, b`, and the no-argument forms.
    private def collect_macro_call_args : Array( Array( Token ) )
      if @current.kind.l_paren?
        advance   # consume `(`
        collect_macro_args
      elsif at_end? || @current.kind.newline? || @current.kind.semicolon? || BODY_TERMINATORS.includes?( @current.kind )
        [] of Array( Token )
      else
        collect_space_macro_args
      end
    end

    # Collect space-call arguments: comma-separated token groups until end of line,
    # honouring bracket nesting so commas inside `(...)`/`[...]` don't split arguments.
    private def collect_space_macro_args : Array( Array( Token ) )
      args    = [] of Array( Token )
      current = [] of Token
      depth   = 0
      until at_end? || ( depth == 0 && ( @current.kind.newline? || @current.kind.semicolon? ) )
        kind = @current.kind
        if kind.l_paren? || kind.l_bracket? || kind.l_brace?
          depth += 1; current << advance
        elsif kind.r_paren? || kind.r_bracket? || kind.r_brace?
          depth -= 1; current << advance
        elsif kind.comma? && depth == 0
          advance
          args << current
          current = [] of Token
        else
          current << advance
        end
      end
      args << current unless current.empty?
      args
    end

    private def can_start_expr?( kind : TokenKind ) : Bool
      case kind
      when .int?, .float?, .string?, .true?, .false?, .nil?, .ident?, .self_?,
            .l_paren?, .l_bracket?, .l_brace?, .minus?, .plus?, .tilde?,
            .dunder_file?, .dunder_line?, .regex?, .bang?, .not?,
            .if?, .unless?, .match?, .while?, .until?, .await?,
            .return?, .break?, .raise?
        true
      else
        false
      end
    end

    private def parse_space_call_args : Array( AExpr )
      args = [] of AExpr
      args << parse_expr( Prec::Assignment )
      while @current.kind.comma?
        advance
        skip_newlines
        args << parse_expr( Prec::Assignment )
      end
      args
    end

  end


end
