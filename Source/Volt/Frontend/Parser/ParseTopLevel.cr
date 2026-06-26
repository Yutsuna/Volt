module Volt::Frontend


  class Parser

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

  end


end
