module Volt::Frontend


  # Monomorphization engine for generic templates (architecture: generics are
  # resolved entirely at semantic time : each concrete use of a generic class
  # or function clones the template's AST with its type parameters substituted
  # and registers an ordinary declaration under a mangled name, so the
  # compiler and VM never see a type parameter).
  #
  # A generic `class Pair[T, U]` or `def identity( value : T ) -> T forall T`
  # is registered here as a *template* and excluded from normal collection.
  # `instantiate_class` / `instantiate_func` produce a concrete clone named
  # `Pair[String, Int64]` / `identity[Int64]` (matching `Type#to_s`, so
  # diagnostics read like source). Instantiation is cached by mangled name :
  # requesting the same combination twice yields one declaration. The fresh
  # clones queue in `fresh_classes` / `fresh_funcs` until the analyser drains
  # them into normal collection/checking : new instantiations discovered while
  # checking a clone simply re-enter the queue (worklist to fixpoint).
  class Monomorphizer
    # A mangled name nests one `[` per generic level; runaway recursive
    # instantiation (`Wrap[Wrap[Wrap[...]]]`) grows that nesting on every
    # round, so capping it bounds the expansion without tracking call chains.
    MAX_NESTING = 8

    getter class_templates : Hash( String, ClassDecl )
    getter func_templates  : Hash( String, FuncDecl )
    getter fresh_classes   : Array( ClassDecl )
    getter fresh_funcs     : Array( FuncDecl )

    def initialize( @bag : DiagnosticBag )
      @class_templates = {} of String => ClassDecl
      @func_templates  = {} of String => FuncDecl
      @fresh_classes   = [] of ClassDecl
      @fresh_funcs     = [] of FuncDecl
      @instantiated    = Set( String ).new
    end

    # -----------------------------------------------------------------------------------

    def register_class_template( decl : ClassDecl ) : Nil
      if first = @class_templates[ decl.name ]?
        @bag << Catalog::Sema.duplicate_definition( decl.name, decl.loc, first.loc )
        return
      end
      @class_templates[ decl.name ] = decl
    end

    def register_func_template( decl : FuncDecl ) : Nil
      if first = @func_templates[ decl.name ]?
        @bag << Catalog::Sema.duplicate_definition( decl.name, decl.loc, first.loc )
        return
      end
      @func_templates[ decl.name ] = decl
    end

    def class_template?( name : String ) : Bool
      @class_templates.has_key?( name )
    end

    def func_template?( name : String ) : Bool
      @func_templates.has_key?( name )
    end

    def class_template( name : String ) : ClassDecl?
      @class_templates[ name ]?
    end

    def func_template( name : String ) : FuncDecl?
      @func_templates[ name ]?
    end

    # True once `mangled` has been produced (used by the REPL delta path to
    # avoid re-declaring an instantiation from a previous input).
    def instantiated?( mangled : String ) : Bool
      @instantiated.includes?( mangled )
    end

    # Marks a combination as already produced without cloning anything : the
    # REPL seeds a fresh Monomorphizer per input and pre-marks every
    # instantiation carried over in the incremental state, so re-using
    # `Pair[String, Int64]` in a later input resolves against the persisted
    # type instead of re-declaring it.
    def mark_instantiated( mangled : String ) : Nil
      @instantiated << mangled
    end

    # -----------------------------------------------------------------------------------

    # Class-level so `Type.from_annotation` can build the same lookup key
    # without holding a Monomorphizer instance.
    def self.mangle( name : String, args : Array( Type ) ) : String
      "#{name}[#{args.map( &.to_s ).join( ", " )}]"
    end

    def mangle( name : String, args : Array( Type ) ) : String
      Monomorphizer.mangle( name, args )
    end

    # Instantiates `Pair[String, Int64]` from the `Pair` template : returns the
    # mangled name, or nil when the request is invalid (a diagnostic is
    # emitted). The concrete `ClassDecl` lands in `fresh_classes` exactly once.
    def instantiate_class( name : String, args : Array( Type ), loc : Span ) : String?
      tmpl = @class_templates[ name ]?
      return nil unless tmpl
      return nil unless validate_args( name, tmpl.type_params.size, args, loc )

      mangled = mangle( name, args )
      return nil unless check_nesting( mangled, loc )
      return mangled if @instantiated.includes?( mangled )
      @instantiated << mangled

      subst = build_subst( tmpl.type_params, args )
      body  = tmpl.body.map { |n| clone_node( n, subst ) }
      clone = ClassDecl.new( mangled, [] of String, tmpl.superclass, tmpl.mixins.dup,
                             body, tmpl.annotations, tmpl.is_abstract, tmpl.loc )
      @fresh_classes << clone
      mangled
    end

    # Instantiates `identity[Int64]` from the `identity` template : same
    # contract as `instantiate_class`, with the clone landing in `fresh_funcs`.
    def instantiate_func( name : String, args : Array( Type ), loc : Span ) : String?
      tmpl = @func_templates[ name ]?
      return nil unless tmpl
      return nil unless validate_args( name, tmpl.type_params.size, args, loc )

      mangled = mangle( name, args )
      return nil unless check_nesting( mangled, loc )
      return mangled if @instantiated.includes?( mangled )
      @instantiated << mangled

      subst = build_subst( tmpl.type_params, args )
      clone = clone_func( tmpl, subst, new_name: mangled )
      @fresh_funcs << clone
      mangled
    end

    private def validate_args( name : String, expected : Int32, args : Array( Type ), loc : Span ) : Bool
      unless args.size == expected
        @bag << Catalog::Sema.generic_arity_mismatch( name, expected, args.size, loc )
        return false
      end
      # An Unknown argument means resolution/inference already failed upstream
      # (and diagnosed there) : instantiating with it would only cascade.
      args.all? { |a| !a.kind.unknown? }
    end

    private def check_nesting( mangled : String, loc : Span ) : Bool
      return true if mangled.count( '[' ) <= MAX_NESTING
      @bag << Catalog::Sema.instantiation_too_deep( mangled, loc )
      false
    end

    private def build_subst( params : Array( String ), args : Array( Type ) ) : Hash( String, Type )
      subst = {} of String => Type
      params.each_with_index { |p, i| subst[ p ] = args[ i ] }
      subst
    end

    # -----------------------------------------------------------------------------------
    # Type-annotation substitution

    # Renders a resolved `Type` back into a fresh annotation node : this is how
    # a substituted `T` re-enters the cloned AST. Nominal types render as their
    # (possibly mangled) name, which `Type.from_annotation` resolves through
    # the nominals table.
    def type_to_node( t : Type, loc : Span ) : ATypeNode
      case t.kind
      when .pointer?
        PointerType.new( type_to_node( t.pointee, loc ), loc )
      when .func?
        params = t.params.map { |p| type_to_node( p, loc ).as( ATypeNode ) }
        ret    = type_to_node( t.ret || Type::NIL, loc )
        FuncType.new( params, ret, loc )
      else
        SimpleType.new( t.to_s, loc )
      end
    end

    private def clone_type( node : ATypeNode, subst : Hash( String, Type ) ) : ATypeNode
      case node
      when SimpleType
        if replacement = subst[ node.name ]?
          type_to_node( replacement, node.loc )
        else
          SimpleType.new( node.name, node.loc )
        end
      when GenericType
        GenericType.new( node.name, node.params.map { |p| clone_type( p, subst ) }, node.loc )
      when PointerType
        PointerType.new( clone_type( node.inner, subst ), node.loc )
      when NilableType
        NilableType.new( clone_type( node.inner, subst ), node.loc )
      when FuncType
        FuncType.new( node.params.map { |p| clone_type( p, subst ) },
                      clone_type( node.return_type, subst ), node.loc )
      else
        node
      end
    end

    private def clone_type?( node : ATypeNode?, subst : Hash( String, Type ) ) : ATypeNode?
      node.try { |n| clone_type( n, subst ) }
    end

    # -----------------------------------------------------------------------------------
    # AST deep clone
    #
    # Every instantiation gets its own body : sharing mutable AST between two
    # instantiations (or with the template) is forbidden, because the checker
    # rewrites call sites in place and the compiler lowers each clone
    # separately. Source spans are preserved from the template so diagnostics
    # inside an instantiation point at the generic source that produced it.

    private def clone_func( decl : FuncDecl, subst : Hash( String, Type ), new_name : String? = nil ) : FuncDecl
      params = decl.params.map do |p|
        Param.new( p.name, clone_type?( p.type_ann, subst ), p.default.try { |d| clone_expr( d, subst ) },
                   p.loc, p.is_ivar )
      end
      clone = FuncDecl.new( new_name || decl.name, [] of String, params,
                            clone_type?( decl.return_type, subst ),
                            decl.body.map { |n| clone_node( n, subst ) },
                            decl.annotations, decl.is_async, decl.loc )
      clone.is_abstract = decl.is_abstract
      clone.is_static   = decl.is_static
      clone.visibility  = decl.visibility
      clone
    end

    private def clone_node( node : ANode, subst : Hash( String, Type ) ) : ANode
      case node
      when AExpr
        clone_expr( node, subst )
      when FuncDecl
        unless node.type_params.empty?
          @bag << Catalog::Sema.generic_method_unsupported( node.name, node.loc )
        end
        clone_func( node, subst )
      when FieldDecl
        clone = FieldDecl.new( node.name, clone_type( node.type_ann, subst ),
                               node.value.try { |v| clone_expr( v, subst ) }, node.loc )
        clone.is_getter = node.is_getter
        clone.is_setter = node.is_setter
        clone
      when ClassVarDecl
        ClassVarDecl.new( node.name, clone_type( node.type_ann, subst ),
                          node.value.try { |v| clone_expr( v, subst ) }, node.loc )
      when IncludeDecl
        IncludeDecl.new( node.name, node.loc )
      when ExtendSelfDecl
        ExtendSelfDecl.new( node.loc )
      else
        # Nested type declarations inside a generic body would re-declare the
        # same name on every instantiation : rejected rather than silently
        # cloned into a duplicate-definition storm.
        @bag << Catalog::Sema.unsupported_in_template( node.class.name.split( "::" ).last, node.loc )
        node
      end
    end

    private def clone_body( body : Array( ANode ), subst : Hash( String, Type ) ) : Array( ANode )
      body.map { |n| clone_node( n, subst ) }
    end

    private def clone_expr?( expr : AExpr?, subst : Hash( String, Type ) ) : AExpr?
      expr.try { |e| clone_expr( e, subst ) }
    end

    private def clone_block( blk : BlockExpr?, subst : Hash( String, Type ) ) : BlockExpr?
      blk.try { |b| BlockExpr.new( b.params.dup, clone_body( b.body, subst ), b.loc ) }
    end

    private def clone_expr( expr : AExpr, subst : Hash( String, Type ) ) : AExpr
      case expr
      when IntLit    then IntLit.new( expr.value, expr.loc )
      when FloatLit  then FloatLit.new( expr.value, expr.loc )
      when StringLit then StringLit.new( expr.value, expr.loc )
      when BoolLit   then BoolLit.new( expr.value, expr.loc )
      when NilLit    then NilLit.new( expr.loc )
      when RegexLit  then RegexLit.new( expr.value, expr.loc )
      when Ident
        # A bare type-parameter reference in expression position (`T.new`)
        # rewrites to the substituted type's name so the constructor path
        # resolves it nominally.
        if replacement = subst[ expr.name ]?
          Ident.new( replacement.to_s, expr.loc )
        else
          Ident.new( expr.name, expr.loc )
        end
      when SelfExpr    then SelfExpr.new( expr.loc )
      when InstanceVar then InstanceVar.new( expr.name, expr.loc )
      when ClassVar    then ClassVar.new( expr.name, expr.loc )
      when ArrayLit
        ArrayLit.new( expr.elements.map { |e| clone_expr( e, subst ) }, expr.loc )
      when HashLiteralExpr
        pairs = expr.pairs.map { |k, v| { clone_expr( k, subst ).as( AExpr ), clone_expr( v, subst ).as( AExpr ) } }
        HashLiteralExpr.new( pairs, expr.loc )
      when SuperCall
        SuperCall.new( expr.args.map { |a| clone_expr( a, subst ) }, expr.implicit_args, expr.loc )
      when MemberAccess
        MemberAccess.new( clone_expr( expr.receiver, subst ), expr.name, expr.safe, expr.loc )
      when BinaryOp
        BinaryOp.new( clone_expr( expr.left, subst ), expr.op, clone_expr( expr.right, subst ), expr.loc )
      when UnaryOp
        UnaryOp.new( expr.op, clone_expr( expr.operand, subst ), expr.loc )
      when Assign
        Assign.new( clone_expr( expr.target, subst ), clone_type?( expr.type_ann, subst ),
                    clone_expr( expr.value, subst ), expr.loc )
      when BlockExpr
        BlockExpr.new( expr.params.dup, clone_body( expr.body, subst ), expr.loc )
      when Call
        Call.new( clone_expr( expr.callee, subst ),
                  expr.args.map { |a| clone_expr( a, subst ) },
                  clone_block( expr.block, subst ), expr.loc )
      when MethodCall
        MethodCall.new( clone_expr( expr.receiver, subst ), expr.name,
                        expr.args.map { |a| clone_expr( a, subst ) },
                        clone_block( expr.block, subst ), expr.safe, expr.loc )
      when Index
        clone = Index.new( clone_expr( expr.receiver, subst ), clone_expr( expr.index, subst ), expr.loc )
        clone.extra_args = expr.extra_args.map { |a| clone_expr( a, subst ) }
        clone
      when PipeExpr
        PipeExpr.new( clone_expr( expr.left, subst ), clone_expr( expr.right, subst ), expr.loc )
      when IfExpr
        elsifs = expr.elsifs.map { |cond, body| { clone_expr( cond, subst ).as( AExpr ), clone_body( body, subst ) } }
        IfExpr.new( clone_expr( expr.cond, subst ), clone_body( expr.then_b, subst ),
                    elsifs, expr.else_b.try { |b| clone_body( b, subst ) }, expr.loc )
      when MatchExpr
        arms = expr.arms.map do |arm|
          MatchArm.new( arm.patterns.map { |p| clone_expr( p, subst ) },
                        clone_expr( arm.body, subst ), arm.is_else )
        end
        MatchExpr.new( clone_expr( expr.value, subst ), arms, expr.loc )
      when RangeExpr
        RangeExpr.new( clone_expr( expr.from, subst ), clone_expr( expr.to, subst ),
                       expr.exclusive, expr.loc )
      when AwaitExpr  then AwaitExpr.new( clone_expr( expr.expr, subst ), expr.loc )
      when ReturnExpr then ReturnExpr.new( clone_expr?( expr.value, subst ), expr.loc )
      when BreakExpr  then BreakExpr.new( clone_expr?( expr.value, subst ), expr.loc )
      when NextExpr   then NextExpr.new( clone_expr?( expr.value, subst ), expr.loc )
      when RaiseExpr  then RaiseExpr.new( clone_expr( expr.value, subst ), expr.loc )
      when WhileExpr
        WhileExpr.new( clone_expr( expr.cond, subst ), clone_body( expr.body, subst ), expr.loc )
      when TypeofExpr
        TypeofExpr.new( clone_expr( expr.operand, subst ), expr.loc )
      else
        @bag << Catalog::Sema.unsupported_in_template( expr.class.name.split( "::" ).last, expr.loc )
        expr
      end
    end
  end


end
