module Volt::Frontend


  class Parser

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

  end


end
