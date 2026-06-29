module Volt::Frontend


  class Parser

    #------------------------------------------------------------------------------------

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

    #------------------------------------------------------------------------------------

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

  end


end
