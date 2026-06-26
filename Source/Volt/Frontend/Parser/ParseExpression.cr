module Volt::Frontend


  class Parser

    private def parse_expr( min_prec : Prec = Prec::None ) : AExpr
      skip_newlines if @paren_depth > 0
      left = nud( advance )
      loop do
        skip_newlines if @paren_depth > 0

        if @paren_depth == 0 && @current.kind.newline?
          continuation = led_prec( @peek.kind )
          break if continuation <= min_prec
          advance
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
        IntLit.new( tok.value.to_i64, tok.span )
      when .float?
        FloatLit.new( tok.value.to_f64, tok.span )
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
        bin_op = case op.kind
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

  end


end
