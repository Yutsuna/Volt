module Volt::Frontend


  class Parser

    #------------------------------------------------------------------------------------

    private def parse_type : ATypeNode
      if @current.kind.star?
        loc = @current.span
        advance
        inner = parse_type
        return PointerType.new( inner, loc )
      end

      loc  = @current.span
      name = expect( TokenKind::Ident ).value

      # Namespace-qualified type name: `SmartCity::SmartDevice` resolves to a
      # single `SimpleType` whose name is the fully-qualified `"A::B"` string.
      while @current.kind.colon_colon?
        advance
        name = "#{name}::#{expect( TokenKind::Ident ).value}"
      end

      # The lexer folds a trailing `?` into the identifier itself (idents may
      # end in `?`/`!` for method names), so `ListNode?` arrives as one Ident
      # token — split the nilable marker back off the type name here. The
      # standalone `?`-token check further down still covers the detached
      # form (e.g. after a generic: `Array[Int]?`).
      glued_nilable = name.ends_with?( '?' )
      name = name.rchop( '?' ) if glued_nilable

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

      ty = NilableType.new( ty, loc ) if glued_nilable

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

  end



end
