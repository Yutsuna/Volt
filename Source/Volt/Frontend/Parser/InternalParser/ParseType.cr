module Volt::Frontend


  class Parser

    #------------------------------------------------------------------------------------

    private def parse_type : ATypeNode
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
        # `Elem[N]` : a fixed-size stack array (`Int64[5]`) reads as an
        # integer literal inside the brackets, distinguishing it from the
        # identifier-based generic-argument form (`Pair[String, Int64]`).
        if @current.kind.int?
          size_tok = advance
          size     = size_tok.value.delete( '_' ).to_i32
          @paren_depth -= 1
          expect( TokenKind::RBracket )
          ArrayType.new( SimpleType.new( name, loc ), size, loc )
        else
          params = [] of ATypeNode
          params << parse_type
          while @current.kind.comma?
            advance; skip_newlines
            params << parse_type
          end
          @paren_depth -= 1
          expect( TokenKind::RBracket )
          GenericType.new( name, params, loc )
        end
      else
        SimpleType.new( name, loc )
      end

      # Must come before nilable handling so Int64*? is (Int64*)?
      while @current.kind.star?
        star_loc = @current.span
        advance
        ty = PointerType.new( ty, star_loc )
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
