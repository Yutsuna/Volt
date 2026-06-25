module Volt::Frontend


  class Parser

    def initialize( source : String, file : String = "<unknown>" )
      @lexer       = Lexer.new( source, file )
      @file        = file
      @current     = @lexer.next_token
      @peek        = @lexer.next_token
      @paren_depth = 0
    end


    def parse : Program
      parse_program
    end

    #------------------------------------------------------------------------------------

    private def advance : Token
      tok      = @current
      @current = @peek
      @peek    = @lexer.next_token
      tok
    end

    private def check( kind : TokenKind ) : Bool
      skip_newlines if @paren_depth > 0
      @current.kind == kind
    end

    private def expect( kind : TokenKind ) : Token
      skip_newlines if @paren_depth > 0
      unless @current.kind == kind
        error!( "expected #{kind}, got `#{@current.value}`", @current.span )
      end
      advance
    end

    private def skip_newlines
      while @current.kind.newline?
        advance
      end
    end

    private def skip_separators
      while @current.kind.newline? || @current.kind.semicolon?
        advance
      end
    end

    private def at_end? : Bool
      @current.kind.eof?
    end

    private def error!( msg : String, span : Span ) : NoReturn
      raise ParseError.new( msg, span )
    end


    #------------------------------------------------------------------------------------


    private def parse_program : Program
      loc   = @current.span
      nodes = [] of ANode
      skip_separators
      until at_end?
        nodes << parse_top_level_node
        skip_separators
      end
      Program.new( nodes, @file, loc )
    end

    private def parse_top_level_node : ANode
      annots = collect_annotations
      case @current.kind
      when .def?
        parse_func_decl( annots, is_async: false )
      when .async?
        loc = advance.span
        if @current.kind.def?
          parse_func_decl( annots, is_async: true )
        elsif @current.kind.component?
          parse_component_decl( annots, is_async: true )
        else
          error!( "expected `def` or `component` after `async`", @current.span )
        end
      when .class?
        parse_class_decl( annots )
      when .mixin?
        parse_mixin_decl
      when .component?
        parse_component_decl( annots, is_async: false )
      when .use?
        parse_use_decl
      else
        parse_expr_node
      end
    end


    #------------------------------------------------------------------------------------


    private def collect_annotations : Array( Annotation )
      annots = [] of Annotation
      while @current.kind.at?
        loc = @current.span
        advance   # consume @
        expect( TokenKind::LBracket )
        name = expect( TokenKind::Ident ).value
        args = [] of AExpr
        if @current.kind.l_paren?
          advance
          @paren_depth += 1
          args = parse_arg_list( TokenKind::RParen )
          @paren_depth -= 1
        end
        expect( TokenKind::RBracket )
        annots << Annotation.new( name, args, loc )
        skip_separators
      end
      annots
    end


    #------------------------------------------------------------------------------------


    private def parse_func_decl( annots : Array( Annotation ), is_async : Bool ) : Decl
      loc = @current.span
      advance   # consume `def`
      name = expect( TokenKind::Ident ).value

      type_params = parse_type_params_if_present
      params      = parse_param_list
      return_type = parse_return_type_if_present

      if annots.any? { |a| a.name == "External" }
        lib_name = annots.find( &.name.==( "External" ) ).try &.args.first?.try { |e|
          e.is_a?( StringLit ) ? e.value : nil
        }
        skip_separators
        return ExternDecl.new( lib_name, name, params, return_type || SimpleType.new( "Void", loc ), loc )
      end

      skip_separators
      body = parse_body
      expect( TokenKind::End )

      FuncDecl.new( name, type_params, params, return_type, body, annots, is_async, loc )
    end

    private def parse_class_decl( annots : Array( Annotation ) ) : ClassDecl
      loc = @current.span
      advance   # consume `class`
      name        = expect( TokenKind::Ident ).value
      type_params = parse_type_params_if_present
      mixins      = [] of String

      while @current.kind.include?
        advance
        mixins << expect( TokenKind::Ident ).value
      end

      skip_separators
      body = parse_body
      expect( TokenKind::End )

      ClassDecl.new( name, type_params, mixins, body, annots, loc )
    end

    private def parse_mixin_decl : MixinDecl
      loc = @current.span
      advance   # consume `mixin`
      name = expect( TokenKind::Ident ).value
      skip_separators
      body = parse_body
      expect( TokenKind::End )
      MixinDecl.new( name, body, loc )
    end

    private def parse_component_decl( annots : Array( Annotation ), is_async : Bool ) : ComponentDecl
      loc = @current.span
      advance   # consume `component`
      name   = expect( TokenKind::Ident ).value
      params = parse_param_list
      skip_separators
      body = parse_body
      expect( TokenKind::End )
      ComponentDecl.new( name, params, body, is_async, loc )
    end

    private def parse_use_decl : UseDecl
      loc = @current.span
      advance   # consume `use`
      path_parts = [] of String
      path_parts << expect( TokenKind::Ident ).value
      while @current.kind.colon? && @peek.kind.colon?
        advance; advance
        path_parts << expect( TokenKind::Ident ).value
      end
      UseDecl.new( path_parts.join( "::" ), loc )
    end


    #------------------------------------------------------------------------------------


    private def parse_param_list : Array( Param )
      return [] of Param unless @current.kind.l_paren?
      advance   # consume (
      @paren_depth += 1
      params = [] of Param
      skip_newlines
      until @current.kind.r_paren? || at_end?
        params << parse_param
        skip_newlines
        break unless @current.kind.comma?
        advance; skip_newlines
      end
      @paren_depth -= 1
      expect( TokenKind::RParen )
      params
    end

    private def parse_param : Param
      loc  = @current.span
      name = expect( TokenKind::Ident ).value
      type_ann = if @current.kind.colon?
        advance; parse_type
      end
      default = if @current.kind.eq?
        advance; parse_expr
      end
      Param.new( name, type_ann, default, loc )
    end

    private def parse_type_params_if_present : Array( String )
      return [] of String unless @current.kind.l_bracket?
      advance
      @paren_depth += 1
      names = [] of String
      names << expect( TokenKind::Ident ).value
      while @current.kind.comma?
        advance; skip_newlines
        names << expect( TokenKind::Ident ).value
      end
      @paren_depth -= 1
      expect( TokenKind::RBracket )
      names
    end

    private def parse_return_type_if_present : ATypeNode?
      return nil unless @current.kind.arrow?
      advance
      parse_type
    end


    #------------------------------------------------------------------------------------


    private def parse_type : ATypeNode
      loc  = @current.span
      name = expect( TokenKind::Ident ).value

      ty : ATypeNode = if @current.kind.l_bracket?
        advance
        @paren_depth += 1
        params = [] of ATypeNode
        params << parse_type
        while @current.kind.comma?
          advance; skip_newlines
          params << parse_type
        end
        @paren_depth -= 1
        expect( TokenKind::RBracket )
        GenericType.new( name, params, loc )
      else
        SimpleType.new( name, loc )
      end

      # T -> U  (function type sugar)
      if @current.kind.arrow?
        advance
        ret = parse_type
        in_params = [] of ATypeNode
        in_params << ty
        return FuncType.new( in_params, ret, loc )
      end

      # T?
      if @current.kind.question?
        advance
        return NilableType.new( ty, loc )
      end

      ty
    end


    #------------------------------------------------------------------------------------


    BODY_TERMINATORS = [ TokenKind::End, TokenKind::Else, TokenKind::Elsif,
                         TokenKind::When, TokenKind::Eof ]

    private def parse_body : Array( ANode )
      nodes = [] of ANode
      skip_separators
      until BODY_TERMINATORS.includes?( @current.kind )
        nodes << parse_body_node
        skip_separators
      end
      nodes
    end

    private def parse_body_node : ANode
      annots = collect_annotations
      case @current.kind
      when .def?
        parse_func_decl( annots, is_async: false )
      when .async?
        advance
        if @current.kind.def?
          parse_func_decl( annots, is_async: true )
        else
          error!( "expected `def` after `async`", @current.span )
        end
      when .class?
        parse_class_decl( annots )
      when .mixin?
        parse_mixin_decl
      else
        parse_expr_node
      end
    end

    private def parse_expr_node : AExpr
      parse_expr
    end


    #------------------------------------------------------------------------------------


    private def parse_expr( min_prec : Prec = Prec::None ) : AExpr
      skip_newlines if @paren_depth > 0
      left = nud( advance )
      loop do
        skip_newlines if @paren_depth > 0

        if @paren_depth == 0 && @current.kind.newline?
          continuation = led_prec( @peek.kind )
          break if continuation <= min_prec
          advance   # consume the newline
        end

        bp = led_prec( @current.kind )
        break if bp <= min_prec
        op   = advance
        left = led( left, op )
      end
      left
    end


    #------------------------------------------------------------------------------------


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
      when .bang?, .not?
        operand = parse_expr( Prec::Unary )
        UnaryOp.new( TokenKind::Bang, operand, tok.span )
      when .if?
        parse_if_expr( tok )
      when .unless?
        parse_unless_expr( tok )
      when .match?
        parse_match_expr( tok )
      when .await?
        AwaitExpr.new( parse_expr( Prec::Unary ), tok.span )
      when .return?
        ReturnExpr.new( expr_if_on_same_line, tok.span )
      when .break?
        BreakExpr.new( expr_if_on_same_line, tok.span )
      when .raise?
        RaiseExpr.new( parse_expr( Prec::Unary ), tok.span )
      else
        error!( "unexpected token `#{tok.value}` in expression", tok.span )
      end
    end


    #------------------------------------------------------------------------------------


    private def led( left : AExpr, op : Token ) : AExpr
      case op.kind
      when .plus?, .minus?, .star?, .slash?, .percent?
        BinaryOp.new( left, op.kind, parse_expr( led_prec( op.kind ) ), left.loc )
      when .star_star?
        BinaryOp.new( left, op.kind, parse_expr( Prec.new( Prec::Power.value - 1 ) ), left.loc )
      when .eq_eq?, .bang_eq?, .lt?, .gt?, .lt_eq?, .gt_eq?
        BinaryOp.new( left, op.kind, parse_expr( led_prec( op.kind ) ), left.loc )
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
        error!( "unexpected infix operator `#{op.value}`", op.span )
      end
    end


    #------------------------------------------------------------------------------------


    private def led_prec( kind : TokenKind ) : Prec
      case kind
      when .eq?, .colon?                         then Prec::Assignment
      when .or?, .pipe_pipe?                     then Prec::Or
      when .and?, .amp_amp?                      then Prec::And
      when .eq_eq?, .bang_eq?                    then Prec::Equality
      when .lt?, .gt?, .lt_eq?, .gt_eq?         then Prec::Comparison
      when .dot_dot?, .dot_dot_dot?              then Prec::Range
      when .pipe_gt?                             then Prec::Pipe
      when .plus?, .minus?                       then Prec::Term
      when .star?, .slash?, .percent?            then Prec::Factor
      when .star_star?                           then Prec::Power
      when .dot?, .question?                     then Prec::Call
      when .l_paren?, .l_bracket?, .l_brace?    then Prec::Call
      else                                        Prec::None
      end
    end


    #------------------------------------------------------------------------------------


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
      ArrayLit.new( elements, tok.span )
    end

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

    private def parse_body_until( terminator : TokenKind ) : Array( ANode )
      nodes = [] of ANode
      skip_separators
      until @current.kind == terminator || at_end?
        nodes << parse_body_node
        skip_separators
      end
      nodes
    end

    private def parse_dot_call( receiver : AExpr, safe : Bool ) : AExpr
      name = expect( TokenKind::Ident ).value
      if @current.kind.l_paren?
        advance
        @paren_depth += 1
        args = parse_arg_list( TokenKind::RParen )
        @paren_depth -= 1
      else
        args = [] of AExpr
      end
      blk = if @current.kind.l_brace?
        parse_brace_block
      end
      MethodCall.new( receiver, name, args, blk, safe, receiver.loc )
    end

    private def parse_call_with_open_paren( callee : AExpr, open_paren : Token ) : Call
      @paren_depth += 1
      args = parse_arg_list( TokenKind::RParen )
      @paren_depth -= 1
      blk = if @current.kind.l_brace?
        parse_brace_block
      end
      Call.new( callee, args, blk, callee.loc )
    end

    private def parse_arg_list( closing : TokenKind ) : Array( AExpr )
      args = [] of AExpr
      skip_newlines
      until @current.kind == closing || at_end?
        args << parse_expr
        skip_newlines
        break unless @current.kind.comma?
        advance; skip_newlines
      end
      expect( closing )
      args
    end


    #------------------------------------------------------------------------------------


    private def parse_if_expr( tok : Token ) : IfExpr
      cond   = parse_expr( Prec::None )
      skip_separators
      then_b = parse_body
      elsifs = [] of { AExpr, Array( ANode ) }
      else_b = nil

      while @current.kind.elsif?
        advance
        ec = parse_expr( Prec::None )
        skip_separators
        eb = parse_body
        elsifs << { ec, eb }
      end

      if @current.kind.else?
        advance
        skip_separators
        else_b = parse_body
      end

      expect( TokenKind::End )
      IfExpr.new( cond, then_b, elsifs, else_b, tok.span )
    end

    private def parse_unless_expr( tok : Token ) : IfExpr
      cond   = parse_expr( Prec::None )
      skip_separators
      body   = parse_body
      else_b = nil
      if @current.kind.else?
        advance; skip_separators
        else_b = parse_body
      end
      expect( TokenKind::End )
      # unless cond → if !cond
      neg = UnaryOp.new( TokenKind::Bang, cond, tok.span )
      IfExpr.new( neg, body, [] of { AExpr, Array( ANode ) }, else_b, tok.span )
    end

    private def parse_match_expr( tok : Token ) : MatchExpr
      value = parse_expr( Prec::None )
      skip_separators
      arms  = [] of MatchArm

      while @current.kind.when? || @current.kind.else?
        if @current.kind.else?
          advance; skip_separators
          body = parse_expr( Prec::None )
          arms << MatchArm.new( [] of AExpr, body, true )
          break
        end
        advance   # consume `when`
        patterns = [] of AExpr
        patterns << parse_expr( Prec::Range )
        while @current.kind.comma?
          advance; patterns << parse_expr( Prec::Range )
        end
        skip_newlines
        advance if @current.kind.then?   # optional `then`
        skip_newlines
        body = parse_expr( Prec::None )
        arms << MatchArm.new( patterns, body, false )
        skip_separators
      end

      expect( TokenKind::End )
      MatchExpr.new( value, arms, tok.span )
    end


    #------------------------------------------------------------------------------------


    private def expr_if_on_same_line : AExpr?
      return nil if @current.kind.newline? || @current.kind.semicolon? || at_end?
      parse_expr( Prec::None )
    end

    private def strip_quotes( tok : Token ) : String
      raw = tok.value
      raw[ 1, raw.bytesize - 2 ]
    end


  end


end
