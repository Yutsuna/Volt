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
      is_ivar = false
      if @current.kind.at?
        advance   # consume @  (constructor shorthand `def initialize( @x : T )`)
        is_ivar = true
      end
      name = expect( TokenKind::Ident ).value
      type_ann = if @current.kind.colon?
        advance; parse_type
      end
      default = if @current.kind.eq?
        advance; parse_expr
      end
      Param.new( name, type_ann, default, loc, is_ivar )
    end

    private def parse_type_params_if_present : Array( String )
      return [] of String unless @current.kind.l_bracket?
      advance
      @paren_depth += 1
      names = [] of String
      names << expect( TokenKind::Ident ).value
      while @current.kind.comma?
        advance; skip_newlines
        tok = expect( TokenKind::Ident )
        if names.includes?( tok.value )
          error!( Catalog::Parse.duplicate_type_param( tok.value, tok ) )
        end
        names << tok.value
      end
      @paren_depth -= 1
      expect( TokenKind::RBracket )
      names
    end

    # Trailing `forall T, U` on a `def` : the Crystal-style spelling of a
    # generic function. `forall` is contextual (an ordinary identifier
    # elsewhere), so it only takes effect in this position. The names merge
    # into the same `type_params` list the `def name[T]` form fills : both
    # spellings declare the same thing, and mixing them may not redeclare a
    # name.
    private def parse_forall_if_present( type_params : Array( String ) ) : Nil
      return unless @current.kind.ident? && @current.value_eq?( "forall" )
      advance   # consume `forall`
      loop do
        tok  = expect( TokenKind::Ident )
        name = tok.value
        if type_params.includes?( name )
          error!( Catalog::Parse.duplicate_type_param( name, tok ) )
        end
        type_params << name
        break unless @current.kind.comma?
        advance; skip_newlines
      end
    end

    private def parse_return_type_if_present : ATypeNode?
      return nil unless @current.kind.arrow?
      advance
      parse_type
    end

    #------------------------------------------------------------------------------------

    # Body of a `class` / `struct` / `mixin` / `module` declaration.
    # Unlike `parse_body`, only declarations are allowed: fields (with optional
    # `getter`/`setter` accessors), methods, and nested types : not statements.
    private def parse_type_body : Array( ANode )
      nodes = [] of ANode
      skip_separators
      until BODY_TERMINATORS.includes?( @current.kind )
        begin
          nodes << parse_type_body_node
        rescue ParseRecovery
          synchronize
        end
        skip_separators
      end
      nodes
    end

    private def parse_type_body_node : ANode
      annots = collect_annotations
      case @current.kind
      when .private?, .protected?, .public?
        parse_visibility_member( annots )
      when .def?
        parse_func_decl( annots, is_async: false )
      when .async?
        advance
        unless @current.kind.def?
          error!( Catalog::Parse.expected_after( "`def`", "async", @current ) )
        end
        parse_func_decl( annots, is_async: true )
      when .abstract?
        advance
        case @current.kind
        when .def?
          parse_func_decl( annots, is_async: false, is_abstract: true )
        when .class?
          parse_class_decl( annots, is_abstract: true )
        else
          error!( Catalog::Parse.expected_after( "`def` or `class`", "abstract", @current ) )
        end
      when .class?
        parse_class_decl( annots )
      when .struct?
        parse_struct_decl( annots )
      when .mixin?
        parse_mixin_decl
      when .module?
        parse_module_decl
      when .include?
        parse_include_decl
      when .extend?
        parse_extend_self
      when .class_var?
        parse_class_var_decl
      when .ident?
        parse_field_decl
      else
        error!( Catalog::Parse.unexpected_in_type_body( @current ) )
      end
    end

    # `private` / `protected` / `public` prefix on a method definition. The
    # modifier applies to the `def` (or `async def` / `abstract def`) that
    # follows; anything else after it is an error.
    private def parse_visibility_member( annots : Array( Annotation ) ) : ANode
      vis = case advance.kind
            when .private?   then Visibility::Private
            when .protected? then Visibility::Protected
            else                  Visibility::Public
            end
      case @current.kind
      when .def?
        parse_func_decl( annots, is_async: false, visibility: vis )
      when .async?
        advance
        unless @current.kind.def?
          error!( Catalog::Parse.expected_after( "`def`", "async", @current ) )
        end
        parse_func_decl( annots, is_async: true, visibility: vis )
      when .abstract?
        advance
        unless @current.kind.def?
          error!( Catalog::Parse.expected_after( "`def`", "abstract", @current ) )
        end
        parse_func_decl( annots, is_async: false, is_abstract: true, visibility: vis )
      else
        error!( Catalog::Parse.expected_after( "`def`", "visibility modifier", @current ) )
      end
    end

    # `extend self` inside a module body.
    private def parse_extend_self : ExtendSelfDecl
      loc = @current.span
      advance   # consume `extend`
      expect( TokenKind::Self_ )
      ExtendSelfDecl.new( loc )
    end

    # `@@name : Type [= default]` module/class variable declaration.
    private def parse_class_var_decl : ClassVarDecl
      loc  = @current.span
      name = advance.value.lchop( "@@" )
      expect( TokenKind::Colon )
      type_ann = parse_type
      value = if @current.kind.eq?
        advance; parse_expr
      end
      ClassVarDecl.new( name, type_ann, value, loc )
    end

    # Body-level `include Mixin` — mirrors the header-level `class Name include Mixin` form.
    private def parse_include_decl : IncludeDecl
      loc = @current.span
      advance   # consume `include`
      name = expect( TokenKind::Ident ).value
      IncludeDecl.new( name, loc )
    end

    # name : Type [= default]  |  getter name : Type  |  setter name : Type
    # `getter` / `setter` are contextual: they only act as accessor markers when
    # followed by another identifier, so a field may still be named `getter`.
    private def parse_field_decl : FieldDecl
      loc       = @current.span
      is_getter = false
      is_setter = false

      while @current.kind.ident? && @peek.kind.ident? &&
            ( @current.value_eq?( "getter" ) || @current.value_eq?( "setter" ) )
        is_getter = true if @current.value_eq?( "getter" )
        is_setter = true if @current.value_eq?( "setter" )
        advance
      end

      name = expect( TokenKind::Ident ).value
      expect( TokenKind::Colon )
      type_ann = parse_type
      value = if @current.kind.eq?
        advance; parse_expr
      end

      decl = FieldDecl.new( name, type_ann, value, loc )
      decl.is_getter = is_getter
      decl.is_setter = is_setter
      decl
    end

  end


end
