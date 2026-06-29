module Volt::Frontend


  class Parser

    private def parse_program : Program
      loc   = @current.span
      nodes = [] of ANode
      collect_nodes( nodes )
      Program.new( nodes, @file, loc )
    end

    private def collect_nodes( nodes : Array( ANode ) ) : Nil
      skip_separators
      until at_end?
        begin
          if @current.kind.macro?
            parse_macro_def
          elsif macro_invocation?
            nodes.concat( expand_macro_statements )
          else
            nodes << parse_top_level_node
          end
        rescue ParseRecovery
          synchronize
        end
        skip_separators
      end
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
          error!( Catalog::Parse.expected_after( "`def` or `component`", "async", @current ) )
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


      private def parse_func_decl( annots : Array( Annotation ), is_async : Bool ) : ADecl
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
          # Extern declarations have no body; an optional `end` is tolerated so both
          # `def puts(...) -> Void` and the `def puts(...) -> Void / end` forms parse.
          advance if @current.kind.end?
          return ExternDecl.new( lib_name, name, params, return_type || SimpleType.new( "Void", loc ), loc )
        end

        skip_separators
        body = parse_body
        expect_close( TokenKind::End, loc, "`def`" )

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
        expect_close( TokenKind::End, loc, "`class`" )

        ClassDecl.new( name, type_params, mixins, body, annots, loc )
      end

      private def parse_mixin_decl : MixinDecl
        loc = @current.span
        advance   # consume `mixin`
        name = expect( TokenKind::Ident ).value
        skip_separators
        body = parse_body
        expect_close( TokenKind::End, loc, "`mixin`" )
        MixinDecl.new( name, body, loc )
      end

      private def parse_component_decl( annots : Array( Annotation ), is_async : Bool ) : ComponentDecl
        loc = @current.span
        advance   # consume `component`
        name   = expect( TokenKind::Ident ).value
        params = parse_param_list
        skip_separators
        body = parse_body
        expect_close( TokenKind::End, loc, "`component`" )
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

      private def parse_macro_def : Nil
        advance   # consume `macro`
        name_tok = expect( TokenKind::Ident )
        name = name_tok.value

        params = [] of String
        if @current.kind.l_paren?
          advance
          @paren_depth += 1
          skip_newlines
          until @current.kind.r_paren? || at_end?
            params << expect( TokenKind::Ident ).value
            skip_newlines
            break unless @current.kind.comma?
            advance; skip_newlines
          end
          @paren_depth -= 1
          expect( TokenKind::RParen )
        end

        body_tokens = [] of Token
        depth = 1
        skip_separators

        until at_end?
          # `{% … %}` directives are opaque to body-depth tracking: their inner keywords
          # (`if`, `for`, `end`...) belong to the macro language, not the surrounding block,
          # so only a bare `end` may close the macro definition.
          if @current.kind.l_macro_expr?
            body_tokens << advance
            until at_end? || @current.kind.r_macro_expr?
              body_tokens << advance
            end
            body_tokens << advance unless at_end?
            next
          end

          tok = advance
          if tok.kind.def? || tok.kind.class? || tok.kind.mixin? || tok.kind.component? ||
             tok.kind.if? || tok.kind.unless? || tok.kind.while? || tok.kind.until? ||
             tok.kind.for? || tok.kind.match? || tok.kind.macro?
            depth += 1
          elsif tok.kind.end?
            depth -= 1
            break if depth == 0
          end
          body_tokens << tok
        end

        @macro_table[ name ] = MacroDef.new( name, params, body_tokens )
      end

      #------------------------------------------------------------------------------------

  end


end
