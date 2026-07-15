module Volt::Frontend


  class TypeChecker
    def initialize( @sigs : SignatureTable, @bag : DiagnosticBag,
                    @types : Hash( String, TypeInfo ) = {} of String => TypeInfo,
                    @nominals : Hash( String, NominalType ) = {} of String => NominalType,
                    @mono : Monomorphizer? = nil, @collector : TypeCollector? = nil )
      @self_type      = nil.as( TypeInfo? )
      @current_method = nil.as( FuncDecl? )
      @loop_depth     = 0
    end

    def check_function( fn : FuncDecl, sig : FuncSig ) : Nil
      scope = Scope.new
      fn.params.each_with_index do |p, i|
        scope.define( p.name, sig.params[ i ]? || Type::UNKNOWN )
      end
      body_ty = check_body( fn.body, scope )
      sig.ret = body_ty if sig.ret.kind.unknown?
    end

    def check_top_level( nodes : Array( ANode ) ) : Nil
      check_body( nodes, Scope.new )
    end

    # `String` resolves nominally once the Core's `class String` has been
    # collected (its `NominalType` is registered under `"String"`, shadowing
    # `Type.from_primitive_name`'s old builtin). Without the Core present
    # (bare unit specs constructing a `TypeChecker` directly), falls back to
    # the legacy `Type::STR` primitive so those specs keep working unchanged.
    private def string_type : Type
      @nominals[ "String" ]? || Type::STR
    end

    # Checks one method body with `self` bound to `owner` : instance-var and
    # implicit-self-call resolution both key off `@self_type`. `abstract def`
    # has no body to check.
    def check_method( fn : FuncDecl, sig : FuncSig, owner : String ) : Nil
      return if fn.is_abstract
      info = @types[ owner ]?
      return unless info

      scope = Scope.new
      fn.params.each_with_index do |p, i|
        scope.define( p.name, sig.params[ i ]? || Type::UNKNOWN )
      end

      previous        = @self_type
      previous_method = @current_method
      @self_type      = info
      @current_method = fn
      check_body( fn.body, scope )
      @self_type      = previous
      @current_method = previous_method
    end

    #------------------------------------------------------------------------------------

    private def check_body( nodes : Array( ANode ), scope : Scope ) : Type
      last = Type::NIL
      nodes.each do |node|
        unless node.is_a?( AExpr )
          @bag << Catalog::Sema.unsupported_nested( type_name( node ), node.loc )
          next
        end
        last = infer( node, scope )
      end
      last
    end

    # -----------------------------------------------------------------------------------

    private def infer( expr : AExpr, scope : Scope ) : Type
      ty = infer_raw( expr, scope )
      expr.resolved_type = ty
      ty
    end

    private def infer_raw( expr : AExpr, scope : Scope ) : Type
      case expr
      when IntLit     then expr.resolved_type || Type::INT
      when FloatLit   then expr.resolved_type || Type::FLOAT
      when StringLit  then string_type
      when BoolLit    then Type::BOOL
      when NilLit     then Type::NIL
      when RegexLit   then Type::REGEX
      when Ident        then infer_ident( expr, scope )
      when InstanceVar  then infer_instance_var( expr, scope )
      when ClassVar     then infer_class_var( expr, scope )
      when MemberAccess then infer_member_access( expr, scope )
      when SelfExpr     then infer_self( expr, scope )
      when SuperCall    then infer_super( expr, scope )
      when Assign     then infer_assign( expr, scope )
      when BinaryOp   then infer_binary( expr, scope )
      when UnaryOp    then infer_unary( expr, scope )
      when Call       then infer_call( expr, scope )
      when IfExpr     then infer_if( expr, scope )
      when WhileExpr  then infer_while( expr, scope )
      when RangeExpr
        infer( expr.from, scope )
        infer( expr.to, scope )
        Type::UNKNOWN
      when MethodCall then infer_method_call( expr, scope )
      when Index      then infer_index( expr, scope )
      when VarDecl    then infer_var_decl( expr, scope )
      when PipeExpr   then infer_pipe( expr, scope )
      when ReturnExpr
        if v = expr.value
          infer( v, scope )
          if local_address_taken?( v, scope )
            @bag << Catalog::Sema.pointer_escape( expr.loc )
          end
        end
        Type::NIL
      when BreakExpr
        @bag << Catalog::Sema.break_outside_loop( expr.loc ) if @loop_depth == 0
        if v = expr.value
          infer( v, scope )
        end
        Type::NIL
      when NextExpr
        @bag << Catalog::Sema.next_outside_loop( expr.loc ) if @loop_depth == 0
        if v = expr.value
          infer( v, scope )
        end
        Type::NIL
      when RaiseExpr
        infer( expr.value, scope )
        Type::UNKNOWN
      when TypeofExpr
        op = expr.operand
        opt = if op.is_a?(Ident) && scope.lookup(op.name)
          infer(op, scope)
        elsif op.is_a?(Ident) && (prim_ty = Type.from_primitive_name(op.name))
          op.resolved_type = prim_ty
          prim_ty
        elsif op.is_a?(Ident) && (nom_ty = @nominals[op.name]?)
          op.resolved_type = nom_ty
          nom_ty
        elsif op.is_a?(Ident) && (sig = @sigs[op.name]?)
          func_ty = Type.func(sig.params, sig.ret)
          op.resolved_type = func_ty
          func_ty
        else
          infer(op, scope)
        end
        expr.resolved_operand_type = opt
        string_type
      when SizeofExpr
        @collector.try(&.instantiate_generics_in( expr.type_node ))
        ty = Type.from_annotation( expr.type_node, @nominals )
        if ty
          expr.byte_size = ty.byte_size
        else
          @bag << Catalog::Sema.unknown_type( "?", expr.loc )
        end
        return Type::INT
      else
        @bag << Catalog::Sema.unsupported_expr( type_name( expr ), expr.loc )
        Type::UNKNOWN
      end
    end

    private def infer_ident( expr : Ident, scope : Scope ) : Type
      if ty = scope.lookup( expr.name )
        return ty
      end
      # A bare identifier inside a method may be a parenthesis-less zero-arg
      # call on `self` (`boot_sequence`), including inherited/mixed-in methods.
      if ( owner = @self_type ) && !scope.local?( expr.name )
        if m = find_method( owner.name, expr.name )
          is_caller_static = @current_method.try( &.is_static ) || false
          if !is_caller_static || m.is_static
            @bag << Catalog::Sema.arity_mismatch( expr.name, m.params.size, 0, expr.loc ) unless m.params.empty?
            return m.ret
          end
        elsif owner.kind.mixin?
          # Same leniency as `infer_call` : a mixin body naming a method it
          # doesn't define resolves against whichever class includes it,
          # which isn't known here. Accept it duck-typed.
          return Type::UNKNOWN
        end
      end
      if sig = @sigs[ expr.name ]?
        if sig.params.size != 0
          @bag << Catalog::Sema.arity_mismatch( expr.name, sig.params.size, 0, expr.loc )
        end
        return sig.ret
      end
      @bag << Catalog::Sema.undefined_variable( expr.name, expr.loc, Suggest.closest( expr.name, scope.visible_names ) )
      Type::UNKNOWN
    end

    # `name : Type` (no initializer) : reserves a fresh local of the
    # annotated type, zero-initialized (see `compile_var_decl` : reuses
    # `NEW_STRUCT`'s existing nil-fill for the multi-slot case). Meaningful
    # for any type, but in practice this is how a stack array is declared —
    # there's no single element value to write `Int64[5] = ...` with.
    private def infer_var_decl( expr : VarDecl, scope : Scope ) : Type
      @collector.try( &.instantiate_generics_in( expr.type_ann ) )
      ty = Type.from_annotation( expr.type_ann, @nominals )
      if ty.nil?
        @bag << Catalog::Sema.unsupported_annotation( expr.type_ann.loc )
        return Type::UNKNOWN
      end
      scope.define( expr.name, ty )
      ty
    end

    private def infer_assign( expr : Assign, scope : Scope ) : Type
      case target = expr.target
      when Ident        then infer_assign_ident( expr, target, scope )
      when InstanceVar  then infer_assign_ivar( expr, target, scope )
      when ClassVar     then infer_assign_class_var( expr, target, scope )
      when MemberAccess then infer_assign_member( expr, target, scope )
      when Index        then infer_assign_index( expr, target, scope )
      when UnaryOp
        if target.op.star?
          infer_assign_deref( expr, target, scope )
        else
          @bag << Catalog::Sema.non_simple_assign( expr.loc )
          infer( expr.value, scope )
          Type::UNKNOWN
        end
      else
        @bag << Catalog::Sema.non_simple_assign( expr.loc )
        infer( expr.value, scope )
        Type::UNKNOWN
      end
    end

    private def infer_assign_deref( expr : Assign, target : UnaryOp, scope : Scope ) : Type
      target_type = infer( target, scope )
      value_type  = infer( expr.value, scope )
      if target_type.kind.unknown?
        return Type::UNKNOWN
      end
      unless type_compatible?( target_type, value_type )
        @bag << Catalog::Sema.annotation_mismatch( "*ptr", target_type.to_s, value_type.to_s, expr.loc )
      end
      target_type
    end

    # `@@name` read : resolves against the enclosing type's class variables.
    private def infer_class_var( expr : ClassVar, scope : Scope ) : Type
      owner = @self_type
      if owner.nil?
        @bag << Catalog::Sema.ivar_outside_method( expr.name, expr.loc )
        return Type::UNKNOWN
      end
      if ty = owner.class_vars[ expr.name ]?
        return ty
      end
      @bag << Catalog::Sema.unknown_instance_var( owner.name, expr.name, expr.loc )
      Type::UNKNOWN
    end

    private def infer_assign_class_var( expr : Assign, target : ClassVar, scope : Scope ) : Type
      value_ty = infer( expr.value, scope )
      if local_address_taken?( expr.value, scope )
        @bag << Catalog::Sema.pointer_escape( expr.loc )
      end
      owner    = @self_type
      if owner.nil?
        @bag << Catalog::Sema.ivar_outside_method( target.name, expr.loc )
        return Type::UNKNOWN
      end
      field_ty = owner.class_vars[ target.name ]?
      if field_ty.nil?
        @bag << Catalog::Sema.unknown_instance_var( owner.name, target.name, expr.loc )
        return Type::UNKNOWN
      end
      unless type_compatible?( field_ty, value_ty )
        @bag << Catalog::Sema.annotation_mismatch( "@@#{target.name}", field_ty.to_s, value_ty.to_s, expr.loc )
      end
      target.resolved_type = field_ty
      field_ty
    end

    private def infer_assign_ident( expr : Assign, target : Ident, scope : Scope ) : Type
      name      = target.name
      value_ty  = infer( expr.value, scope )
      static_ty = value_ty

      if ann = expr.type_ann
        @collector.try( &.instantiate_generics_in( ann ) )
        declared = Type.from_annotation( ann, @nominals )
        if declared.nil?
          @bag << Catalog::Sema.unsupported_annotation( ann.loc )
        elsif !type_compatible?( declared, value_ty )
          @bag << Catalog::Sema.annotation_mismatch( name, declared.to_s, value_ty.to_s, expr.loc )
        else
          # The variable's static type is the (possibly wider) annotation :
          # `usb : Device = UsbDrive.new(...)` reads back as `Device`.
          static_ty = declared
        end
      end

      if existing = scope.lookup( name )
        unless type_compatible?( existing, value_ty )
          @bag << Catalog::Sema.reassign_type( name, existing.to_s, value_ty.to_s, expr.loc )
        end
        if existing.is_a?(NominalType)
          static_ty = existing
        else
          static_ty = value_ty
        end
      end

      scope.define( name, static_ty )
      target.resolved_type = static_ty
      static_ty
    end

    private def infer_assign_ivar( expr : Assign, target : InstanceVar, scope : Scope ) : Type
      value_ty = infer( expr.value, scope )
      if local_address_taken?( expr.value, scope )
        @bag << Catalog::Sema.pointer_escape( expr.loc )
      end
      owner    = @self_type
      if owner.nil?
        @bag << Catalog::Sema.ivar_outside_method( target.name, expr.loc )
        return Type::UNKNOWN
      end

      field = owner.layout.try( &.field?( target.name ) )
      if field.nil?
        # Mixin bodies reference the includer's instance state (see
        # infer_instance_var below) : accept leniently on the write path too.
        return Type::UNKNOWN if owner.kind.mixin?

        @bag << Catalog::Sema.unknown_instance_var( owner.name, target.name, expr.loc )
        return Type::UNKNOWN
      end

      unless type_compatible?( field.type, value_ty )
        @bag << Catalog::Sema.annotation_mismatch( "@#{target.name}", field.type.to_s, value_ty.to_s, expr.loc )
      end
      target.resolved_type = field.type
      field.type
    end

    private def infer_assign_member( expr : Assign, target : MemberAccess, scope : Scope ) : Type
      recv_ty  = infer( target.receiver, scope )
      value_ty = infer( expr.value, scope )
      if local_address_taken?( expr.value, scope )
        @bag << Catalog::Sema.pointer_escape( expr.loc )
      end
      return Type::UNKNOWN unless recv_ty.is_a?( NominalType )

      field = find_field( recv_ty.name, target.name )
      if field.nil?
        # No plain field named `target.name` : fall back to a user-defined
        # `name=` setter method (`def value=( val : T ) -> T`), mirroring
        # `infer_assign_index`'s `[]=` dispatch below.
        if sig = find_method( recv_ty.name, "#{target.name}=" )
          check_visibility( sig, target.receiver, recv_ty.name, expr.loc )
          check_call_args( "#{recv_ty}##{target.name}=", sig.params, [ expr.value ], expr.loc, scope, [ value_ty ] )
          expr.is_setter_call = true
          return sig.ret
        end
        @bag << Catalog::Sema.unknown_field_or_method( recv_ty.name, target.name, expr.loc )
        return Type::UNKNOWN
      end

      unless type_compatible?( field.type, value_ty )
        @bag << Catalog::Sema.annotation_mismatch( target.name, field.type.to_s, value_ty.to_s, expr.loc )
      end
      field.type
    end

    private def infer_instance_var( expr : InstanceVar, scope : Scope ) : Type
      owner = @self_type
      if owner.nil?
        @bag << Catalog::Sema.ivar_outside_method( expr.name, expr.loc )
        return Type::UNKNOWN
      end

      if field = owner.layout.try( &.field?( expr.name ) )
        return field.type
      end

      # Mixin bodies reference the includer's instance state, which isn't
      # known until a concrete class mixes them in : accept leniently rather
      # than force every mixin to redeclare the fields it expects.
      return Type::UNKNOWN if owner.kind.mixin?

      @bag << Catalog::Sema.unknown_instance_var( owner.name, expr.name, expr.loc )
      Type::UNKNOWN
    end

    private def infer_self( expr : SelfExpr, scope : Scope ) : Type
      if ( owner = @self_type ) && !@current_method.try( &.is_static )
        if owner.name.starts_with?("Pointer[")
          return reconstruct_type(owner.name)
        end
        @nominals[ owner.name ]? || Type.from_primitive_name( owner.name ) || Type::UNKNOWN
      else
        @bag << Catalog::Sema.self_outside_method( expr.loc )
        Type::UNKNOWN
      end
    end

    private def reconstruct_type(name : String) : Type
      if name.starts_with?("Pointer[")
        inner = reconstruct_type(name[8...-1])
        return Type.pointer(inner)
      end
      Type.from_primitive_name(name) || @nominals[name]? || Type::UNKNOWN
    end

    # `super` resolves against the *enclosing method's* name on the superclass
    # chain. Only meaningful inside a class's instance method : a mixin method
    # can't know its includer's parent, a struct/module has no parent at all.
    # A bare `super` forwards the enclosing method's parameters : they're
    # materialised here as `Ident` args (the params are in scope under the
    # same names in both this checker and `FunctionEmiter`).
    private def infer_super( expr : SuperCall, scope : Scope ) : Type
      owner = @self_type
      fn    = @current_method
      if owner.nil? || fn.nil?
        @bag << Catalog::Sema.super_outside_method( expr.loc )
        expr.args.each { |a| infer( a, scope ) }
        return Type::UNKNOWN
      end

      sup = owner.superclass
      if sup.nil? || !owner.kind.class?
        @bag << Catalog::Sema.super_without_parent( owner.name, expr.loc )
        expr.args.each { |a| infer( a, scope ) }
        return Type::UNKNOWN
      end

      # A bare `super` forwards the enclosing params; materialise them first
      # so an `initialize` super can resolve its parent overload by arity.
      if expr.implicit_args
        expr.args          = fn.params.map { |p| Ident.new( p.name, expr.loc ).as( AExpr ) }
        expr.implicit_args = false
      end

      arg_tys = expr.args.map { |a| infer( a, scope ) }

      m =
        if fn.name == "initialize"
          find_super_initializer( sup, expr.args.size, arg_tys, expr.loc )
        else
          find_super_target( sup, fn.name )
        end
      if m.nil?
        @bag << Catalog::Sema.super_no_parent_method( owner.name, fn.name, expr.loc )
        return Type::UNKNOWN
      end

      check_call_args( "super", m.params, expr.args, expr.loc, scope, arg_tys )
      m.ret
    end

    # `super` inside `initialize` targets the nearest ancestor that declares
    # any constructor, then picks its overload by arg count and, when several
    # overloads share that arity, by parameter type (`select_overload`). A
    # declaring ancestor with no matching arity is a hard arity error here
    # (returning `nil` would misreport it as "no ancestor defines initialize").
    private def find_super_initializer( type_name : String?, arity : Int32, arg_tys : Array( Type ), loc : Span ) : FuncSig?
      cur = type_name
      while cur && ( info = @types[ cur ]? )
        unless info.initializers.empty?
          if group = info.initializers[ arity ]?
            return Frontend.select_overload( group, arg_tys )
          end
          expected = info.initializer.try( &.params.size ) || 0
          @bag << Catalog::Sema.arity_mismatch( "super", expected, arity, loc )
          return FuncSig.new( "initialize", Array( Type ).new( arity, Type::UNKNOWN ), Type::NIL )
        end
        cur = info.superclass
      end
      nil
    end

    # Nearest ancestor (starting at `type_name`, walking up) that will actually
    # have a compiled chunk for `name` : its own concrete method, or one
    # adopted from a mixin when it declares none itself (mirroring
    # `BytecodeCompiler#collect_method_jobs`). An `abstract def` has no body,
    # so it can't be a `super` target and the walk continues past it.
    private def find_super_target( type_name : String?, name : String ) : FuncSig?
      cur = type_name
      while cur && ( info = @types[ cur ]? )
        if decl = info.methods_ast[ name ]?
          return info.methods[ name ]? unless decl.is_abstract
        else
          info.mixins.each do |mx|
            if ( mi = @types[ mx ]? ) && ( md = mi.methods_ast[ name ]? ) && !md.is_abstract
              return mi.methods[ name ]?
            end
          end
        end
        cur = info.superclass
      end
      nil
    end

    private def infer_member_access( expr : MemberAccess, scope : Scope ) : Type

      # `Pair[String, Int64].new` (bare, no parens) : explicit generic instantiation.
      if ( recv = expr.receiver ).is_a?( Index ) && generic_template_index?( recv ) && expr.name == "new"
        info = resolve_generic_receiver( recv )
        return Type::UNKNOWN unless info
        expr.receiver = ident_for( info, recv.loc )
        dummy_call = MethodCall.new( expr.receiver, expr.name, [] of AExpr, nil, expr.safe, expr.loc )
        return infer_constructor_call( info, dummy_call, scope )
      end

      # A bare template name has no concrete layout : without constructor
      # arguments there is nothing to infer the type arguments from.
      if ( recv = expr.receiver ).is_a?( Ident ) && !scope.local?( recv.name ) &&
         ( mono = @mono ) && mono.type_template?( recv.name )
        @bag << Catalog::Sema.generic_needs_type_args( recv.name, expr.loc )
        return Type::UNKNOWN
      end

      # `Type.new` : the receiver names a declared class/struct rather than a value in
      if ( recv = expr.receiver ).is_a?( Ident ) && !scope.local?( recv.name ) && ( info = @types[ recv.name ]? )
        recv.resolved_type = @nominals[ recv.name ]?
        if expr.name == "new"
          dummy_call = MethodCall.new( expr.receiver, expr.name, [] of AExpr, nil, expr.safe, expr.loc )
          return infer_constructor_call( info, dummy_call, scope )
        end
      end

      # `Module.method` : the receiver names a declared module rather than a value in
      if ( recv = expr.receiver ).is_a?( Ident ) && !scope.local?( recv.name ) &&
         ( info = @types[ recv.name ]? ) && info.kind.module?
        recv.resolved_type = @nominals[ recv.name ]?
        return infer_static_member( info, expr.name, [] of AExpr, expr.loc, scope )
      end

      recv_ty = infer( expr.receiver, scope )
      return Type::UNKNOWN if recv_ty.kind.unknown?

      # `obj.field` without parens/block is how the parser spells both a field
      # read *and* a zero-arg method call without parens (`book.description`);
      # `.to_string` is the one builtin every value supports ahead of a fuller
      # builtin method table.
      unless recv_ty.is_a?( NominalType )
        # `.size` on a fixed-size stack array is a compiler intrinsic (the
        # length is baked into the *type*, `Type#array_size` — there is no
        # per-instance field to read, so no method dispatch is involved).
        return Type::UINT64 if recv_ty.array? && expr.name == "size"
        if sig = primitive_method( recv_ty, expr.name )
          @bag << Catalog::Sema.arity_mismatch( expr.name, sig.params.size, 0, expr.loc ) unless sig.params.empty?
          return sig.ret
        end
        return string_type if expr.name == "to_string"
        @bag << Catalog::Sema.unsupported_expr( "member access `.#{expr.name}` on non-object type `#{recv_ty}`", expr.loc )
        return Type::UNKNOWN
      end

      if field = find_field( recv_ty.name, expr.name )
        return field.type
      end

      if sig = find_method( recv_ty.name, expr.name )
        @bag << Catalog::Sema.arity_mismatch( expr.name, sig.params.size, 0, expr.loc ) unless sig.params.empty?
        check_visibility( sig, expr.receiver, recv_ty.name, expr.loc )
        return sig.ret
      end

      return string_type if expr.name == "to_string"

      @bag << Catalog::Sema.unknown_field_or_method( recv_ty.name, expr.name, expr.loc )
      Type::UNKNOWN
    end

    private def infer_method_call( expr : MethodCall, scope : Scope ) : Type
      if expr.name == "includes?" && expr.receiver.is_a?( RangeExpr )
        infer( expr.receiver, scope )
        expr.args.each { |a| infer( a, scope ) }
        return Type::BOOL
      end

      # `Pair[String, Int64].new( args )` : explicit generic instantiation. The
      # receiver rewrites to the mangled concrete name so the compiler lowers
      # an ordinary constructor call.
      if ( recv = expr.receiver ).is_a?( Index ) && generic_template_index?( recv )
        info = resolve_generic_receiver( recv )
        unless info
          expr.args.each { |a| infer( a, scope ) }
          return Type::UNKNOWN
        end
        expr.receiver = ident_for( info, recv.loc )
        if expr.name == "new"
          return infer_constructor_call( info, expr, scope )
        else
          return infer_static_member( info, expr.name, expr.args, expr.loc, scope )
        end
      end

      # `Pair.new( "Volt", 2026 )` : type arguments inferred from the
      # constructor arguments, Crystal-style.
      if ( recv = expr.receiver ).is_a?( Ident ) && !scope.local?( recv.name ) &&
         ( mono = @mono ) && mono.type_template?( recv.name )
        unless expr.name == "new"
          @bag << Catalog::Sema.generic_needs_type_args( recv.name, expr.loc )
          expr.args.each { |a| infer( a, scope ) }
          return Type::UNKNOWN
        end
        return infer_inferred_constructor( recv.name, expr, scope )
      end

      # `Type.new( args )` : the receiver names a declared class/struct rather
      # than a value in scope.
      if ( recv = expr.receiver ).is_a?( Ident ) && !scope.local?( recv.name ) && ( info = @types[ recv.name ]? )
        recv.resolved_type = @nominals[ recv.name ]?
        if expr.name == "new"
          return infer_constructor_call( info, expr, scope )
        end
        if info.kind.module?
          return infer_static_member( info, expr.name, expr.args, expr.loc, scope )
        end
        @bag << Catalog::Sema.unknown_field_or_method( recv.name, expr.name, expr.loc )
        expr.args.each { |a| infer( a, scope ) }
        return Type::UNKNOWN
      end

      recv_ty = infer( expr.receiver, scope )
      return Type::UNKNOWN if recv_ty.kind.unknown?

      # `.size` on a fixed-size stack array (see the identical intercept in
      # `infer_member_access`) — covers the explicit-call spelling `arr.size()`.
      if recv_ty.array? && expr.name == "size" && expr.args.empty?
        return Type::UINT64
      end

      # A reopened primitive's own method wins over the builtin fallback :
      # `42.to_string` must run the Core `Int#to_string`, not `TO_STRING`.
      if sig = primitive_method( recv_ty, expr.name )
        check_call_args( "#{recv_ty}##{expr.name}", sig.params, expr.args, expr.loc, scope )
        return sig.ret
      end

      # Minimal builtin: `.to_string` is usable on any value (the samples rely
      # on it for string-concatenation logging) ahead of a fuller builtin table.
      if expr.name == "to_string" && expr.args.empty?
        return string_type
      end

      unless recv_ty.is_a?( NominalType )
        @bag << Catalog::Sema.unsupported_expr( "method call `.#{expr.name}` on non-object type `#{recv_ty}`", expr.loc )
        expr.args.each { |a| infer( a, scope ) }
        return Type::UNKNOWN
      end

      # `find_method` excludes `initialize`/`finalize` since neither dispatches
      # virtually — but an explicit `obj.finalize()` (early/manual teardown) is
      # still a legitimate call, resolved statically to the receiver's own (or
      # nearest ancestor's) `finalize`, the same walk `super` uses.
      if expr.name == "finalize" && expr.args.empty?
        if fsig = find_super_target( recv_ty.name, "finalize" )
          return fsig.ret
        end
      end

      sig = find_method( recv_ty.name, expr.name )
      if sig.nil?
        @bag << Catalog::Sema.unknown_field_or_method( recv_ty.name, expr.name, expr.loc )
        expr.args.each { |a| infer( a, scope ) }
        return Type::UNKNOWN
      end

      check_visibility( sig, expr.receiver, recv_ty.name, expr.loc )
      check_call_args( expr.name, sig.params, expr.args, expr.loc, scope )
      sig.ret
    end

    # `receiver[ index ]` desugars to a call on the receiver's own `[]` method
    # (Crystal-style operator overload) : any `class`/`struct` declaring one
    # (e.g. the Core's `Array[T]`) becomes indexable with no further compiler
    # support. A generic-template reference (`Pair[String, Int64]`) never
    # reaches here : `infer_method_call`/`infer_constructor_call` intercept
    # that shape ahead of general expression inference.
    private def infer_index( expr : Index, scope : Scope ) : Type
      recv_ty  = infer( expr.receiver, scope )
      index_ty = infer( expr.index, scope )
      return Type::UNKNOWN if recv_ty.kind.unknown?

      # `arr[i]` on a fixed-size stack array is a compiler intrinsic : length
      # and element type are both known statically (`Type#array_size`/
      # `#element`), so this never dispatches through a `[]` method — a
      # literal out-of-range index is caught here at compile time ; a
      # runtime-computed one is guarded by `CHECK_INDEX` in the compiler.
      if recv_ty.array?
        unless index_ty.integer?
          @bag << Catalog::Sema.argument_type( 1, "[]", "Int", index_ty.to_s, expr.loc )
        end
        if ( lit = expr.index ).is_a?( IntLit ) && ( lit.value < 0 || lit.value >= recv_ty.array_size )
          @bag << Catalog::Sema.unsupported_expr(
            "array index #{lit.value} out of bounds for size #{recv_ty.array_size}", expr.loc )
        end
        return recv_ty.element
      end

      unless recv_ty.is_a?( NominalType )
        @bag << Catalog::Sema.unsupported_expr( "indexing `[]` on non-object type `#{recv_ty}`", expr.loc )
        return Type::UNKNOWN
      end

      sig = find_method( recv_ty.name, "[]" )
      if sig.nil?
        @bag << Catalog::Sema.unknown_field_or_method( recv_ty.name, "[]", expr.loc )
        return Type::UNKNOWN
      end
      check_call_args( "#{recv_ty}#[]", sig.params, [ expr.index ], expr.loc, scope, [ index_ty ] )
      sig.ret
    end

    # `receiver[ index ] = value` desugars to a call on the receiver's own
    # `[]=` method, mirroring `infer_index` above.
    private def infer_assign_index( expr : Assign, target : Index, scope : Scope ) : Type
      recv_ty  = infer( target.receiver, scope )
      index_ty = infer( target.index, scope )
      value_ty = infer( expr.value, scope )
      return Type::UNKNOWN if recv_ty.kind.unknown?

      if recv_ty.array?
        unless index_ty.integer?
          @bag << Catalog::Sema.argument_type( 1, "[]=", "Int", index_ty.to_s, expr.loc )
        end
        if ( lit = target.index ).is_a?( IntLit ) && ( lit.value < 0 || lit.value >= recv_ty.array_size )
          @bag << Catalog::Sema.unsupported_expr(
            "array index #{lit.value} out of bounds for size #{recv_ty.array_size}", expr.loc )
        end
        unless type_compatible?( recv_ty.element, value_ty )
          @bag << Catalog::Sema.annotation_mismatch( "[]=", recv_ty.element.to_s, value_ty.to_s, expr.loc )
        end
        return recv_ty.element
      end

      unless recv_ty.is_a?( NominalType )
        @bag << Catalog::Sema.unsupported_expr( "indexed assignment `[]=` on non-object type `#{recv_ty}`", expr.loc )
        return Type::UNKNOWN
      end

      sig = find_method( recv_ty.name, "[]=" )
      if sig.nil?
        @bag << Catalog::Sema.unknown_field_or_method( recv_ty.name, "[]=", expr.loc )
        return Type::UNKNOWN
      end
      check_call_args( "#{recv_ty}#[]=", sig.params, [ target.index, expr.value ], expr.loc, scope, [ index_ty, value_ty ] )
      sig.ret
    end

    # `Module.method( args )` : a module is a static namespace, so every one of
    # its methods is callable on the module itself (there are no instances). A
    # `private` static method is reachable only from within the same module.
    private def infer_static_member( info : TypeInfo, name : String, args : Array( AExpr ), loc : Span, scope : Scope ) : Type
      sig = info.methods[ name ]?
      if sig.nil? || !sig.is_static
        @bag << Catalog::Sema.unknown_field_or_method( info.name, name, loc )
        args.each { |a| infer( a, scope ) }
        return Type::UNKNOWN
      end
      if sig.visibility.private? && @self_type.try( &.name ) != info.name
        @bag << Catalog::Sema.private_method_call( info.name, name, loc )
      end
      check_call_args( "#{info.name}.#{name}", sig.params, args, loc, scope )
      sig.ret
    end

    # Access control for a method reached through an *explicit* receiver
    # (`obj.m` / `self.m`). Unqualified self-dispatch (`m`) never lands here and
    # is always permitted. `protected` = same class or a subclass (C++ family);
    # `private` = the receiving instance itself (`self`) only.
    private def check_visibility( sig : FuncSig, receiver : AExpr, recv_type : String, loc : Span ) : Nil
      return if sig.visibility.public?
      owner = sig.owner || recv_type
      if sig.visibility.private?
        unless receiver.is_a?( SelfExpr ) && @self_type.try( &.name ) == owner
          @bag << Catalog::Sema.private_method_call( recv_type, sig.name, loc )
        end
      else
        @bag << Catalog::Sema.protected_method_call( recv_type, sig.name, loc ) unless within_family?( owner )
      end
    end

    # True when the current `self` type is `owner` or one of its subclasses.
    private def within_family?( owner : String ) : Bool
      name = @self_type.try( &.name )
      while name
        return true if name == owner
        name = @types[ name ]?.try( &.superclass )
      end
      false
    end

    private def infer_constructor_call( info : TypeInfo, expr : MethodCall, scope : Scope,
                                        arg_tys : Array( Type )? = nil ) : Type
      # `Pointer[T].new( address )` : compiler intrinsic. A raw pointer is a
      # single tagged word, so constructing one from an integer address is a
      # free reinterpret (mirrors Crystal's `Pointer(T).new(UInt64)`) — there
      # is no field layout or initializer chunk to dispatch to.
      if info.name.starts_with?( "Pointer[" ) && expr.args.size == 1
        tys = arg_tys || expr.args.map { |a| infer( a, scope ) }
        unless tys[ 0 ].integer?
          @bag << Catalog::Sema.argument_type( 1, "#{info.name}.new", "UInt64", tys[ 0 ].to_s, expr.loc )
        end
        pointee = find_method( info.name, "value" ).try( &.ret ) || Type::UNKNOWN
        return Type.pointer( pointee )
      end

      @bag << Catalog::Sema.abstract_instantiation( info.name, expr.loc ) if info.is_abstract

      if owner = find_initializer_owner_info( info.name )
        tys = arg_tys || expr.args.map { |a| infer( a, scope ) }
        if group = owner.initializers[ expr.args.size ]?
          init = Frontend.select_overload( group, tys )
          check_call_args( "#{info.name}.new", init.params, expr.args, expr.loc, scope, tys )
        else
          expected = owner.initializer.try( &.params.size ) || 0
          @bag << Catalog::Sema.arity_mismatch( "#{info.name}.new", expected, expr.args.size, expr.loc )
        end
      elsif layout = info.layout
        check_call_args( "#{info.name}.new", layout.fields.map( &.type ), expr.args, expr.loc, scope, arg_tys )
      else
        expr.args.each { |a| infer( a, scope ) } unless arg_tys
      end

      @nominals[ info.name ]? || Type::UNKNOWN
    end

    # `arg_tys`, when given, carries the argument types a generic-inference
    # path already inferred : reusing them avoids inferring (and diagnosing)
    # each argument expression twice.
    private def check_call_args( name : String, params : Array( Type ), args : Array( AExpr ), loc : Span,
                                 scope : Scope, arg_tys : Array( Type )? = nil ) : Nil
      unless params.size == args.size
        @bag << Catalog::Sema.arity_mismatch( name, params.size, args.size, loc )
      end
      args.each_with_index do |arg, i|
        at    = arg_tys.try( &.[ i ]? ) || infer( arg, scope )
        param = params[ i ]?
        next if param.nil?
        next if type_compatible?( param, at )
        @bag << Catalog::Sema.argument_type( i + 1, name, param.to_s, at.to_s, arg.loc )
      end
    end

    # -----------------------------------------------------------------------------------
    # Generics : instantiation triggers
    #
    # All three surface forms land here : `Pair[String, Int64].new` (explicit
    # class), `Pair.new( "Volt", 2026 )` (inferred class), and generic function
    # calls (`identity( 42 )` inferred, `identity[Int64]( 42 )` explicit).
    # Every path ends by rewriting the call site AST to the mangled concrete
    # name, so the compiler only ever sees ordinary declarations.

    private def generic_template_index?( recv : Index ) : Bool
      base = recv.receiver
      base.is_a?( Ident ) && ( @mono.try( &.type_template?( base.name ) ) || false )
    end

    # Instantiates whichever kind of type template (class or struct) owns
    # `name` and drains the resulting declarations into the shared type
    # tables : returns the mangled name, nil on failure (the monomorphizer
    # diagnosed it).
    private def instantiate_type!( name : String, args : Array( Type ), loc : Span ) : String?
      mono      = @mono
      collector = @collector
      return nil unless mono && collector
      mangled = mono.instantiate_type( name, args, loc )
      return nil unless mangled
      collector.drain_instantiations
      mangled
    end

    # Resolves an expression in type-argument position. `Pair[String, Int64]`
    # parses its arguments as ordinary expressions, so a type argument arrives
    # as an `Ident` (a type name) or a nested `Index` (a nested generic).
    private def type_arg_from_expr( e : AExpr ) : Type?
      case e
      when Ident
        Type.from_primitive_name( e.name ) || @nominals[ e.name ]?
      when Index
        base = e.receiver
        return nil unless base.is_a?( Ident )
        args = [] of Type
        ( [ e.index ] + e.extra_args ).each do |x|
          t = type_arg_from_expr( x )
          return nil unless t
          args << t
        end
        mangled = instantiate_type!( base.name, args, e.loc )
        return nil unless mangled
        @nominals[ mangled ]?
      else
        nil
      end
    end

    # `Pair[String, Int64]` as a constructor receiver : resolves the type
    # arguments and instantiates. Returns the concrete `TypeInfo`, or nil
    # after diagnosing.
    private def resolve_generic_receiver( recv : Index ) : TypeInfo?
      base = recv.receiver
      return nil unless base.is_a?( Ident )
      args = [] of Type
      ( [ recv.index ] + recv.extra_args ).each do |e|
        t = type_arg_from_expr( e )
        if t.nil?
          @bag << Catalog::Sema.unknown_type_argument( e.is_a?( Ident ) ? e.name : "?", e.loc )
          return nil
        end
        args << t
      end
      mangled = instantiate_type!( base.name, args, recv.loc )
      return nil unless mangled
      @types[ mangled ]?
    end

    private def ident_for( info : TypeInfo, loc : Span ) : Ident
      ident = Ident.new( info.name, loc )
      ident.resolved_type = @nominals[ info.name ]?
      ident
    end

    # Binds type parameters by structurally matching a declared annotation
    # against an actual argument type : `T` binds directly, `T*` binds through
    # one pointer level. First binding wins; a conflicting second argument is
    # caught by the ordinary argument check after instantiation.
    private def unify_type_param( ann : ATypeNode?, actual : Type, params : Array( String ),
                                  bindings : Hash( String, Type ) ) : Nil
      return if actual.kind.unknown?
      if ann.is_a?( SimpleType )
        if params.includes?( ann.name ) && !bindings.has_key?( ann.name )
          bindings[ ann.name ] = actual
        end
      elsif ann.is_a?( PointerType )
        unify_type_param( ann.inner, actual.pointee, params, bindings ) if actual.pointer?
      elsif ann.is_a?( NilableType )
        unify_type_param( ann.inner, actual, params, bindings )
      end
    end

    # The annotations to unify constructor arguments against : the template's
    # arity-matching `initialize`, or (constructor-less class/struct) its
    # field declarations in order. Works from the raw body/name so it applies
    # equally to a `ClassDecl` and a `StructDecl` template.
    private def constructor_param_annotations( body : Array( ANode ), arity : Int32 ) : Array( ATypeNode? )?
      inits = body.select { |n| n.is_a?( FuncDecl ) && n.name == "initialize" }.map( &.as( FuncDecl ) )
      if init = inits.find { |fn| fn.params.size == arity }
        return init.params.map( &.type_ann )
      end
      return nil unless inits.empty?
      fields = body.select( FieldDecl )
      return nil unless fields.size == arity
      fields.map { |f| f.type_ann.as( ATypeNode? ) }
    end

    # `Pair.new( "Volt", 2026 )` : infers the type arguments from the
    # constructor arguments, instantiates, and rewrites the receiver. Works
    # for either a generic `class` or a generic `struct` template.
    private def infer_inferred_constructor( name : String, expr : MethodCall, scope : Scope ) : Type
      mono = @mono
      return Type::UNKNOWN unless mono
      type_params = mono.type_template_params( name )
      body        = mono.type_template_body( name )
      return Type::UNKNOWN unless type_params && body

      arg_tys = expr.args.map { |a| infer( a, scope ) }

      anns = constructor_param_annotations( body, expr.args.size )
      if anns.nil?
        @bag << Catalog::Sema.cannot_infer_type_param( type_params.first? || "T", name, expr.loc )
        return Type::UNKNOWN
      end

      bindings = {} of String => Type
      anns.each_with_index do |ann, i|
        at = arg_tys[ i ]?
        unify_type_param( ann, at, type_params, bindings ) if at
      end

      args = [] of Type
      type_params.each do |tp|
        bound = bindings[ tp ]?
        if bound.nil? || bound.kind.unknown?
          @bag << Catalog::Sema.cannot_infer_type_param( tp, name, expr.loc )
          return Type::UNKNOWN
        end
        args << bound
      end

      mangled = instantiate_type!( name, args, expr.loc )
      return Type::UNKNOWN unless mangled
      info = @types[ mangled ]?
      return Type::UNKNOWN unless info
      expr.receiver = ident_for( info, expr.receiver.loc )
      infer_constructor_call( info, expr, scope, arg_tys )
    end

    # `identity( 42 )` : type arguments inferred from the call's arguments.
    private def infer_generic_call( tmpl : FuncDecl, expr : Call, scope : Scope ) : Type
      arg_tys = expr.args.map { |a| infer( a, scope ) }
      unless tmpl.params.size == expr.args.size
        @bag << Catalog::Sema.arity_mismatch( tmpl.name, tmpl.params.size, expr.args.size, expr.loc )
        return Type::UNKNOWN
      end

      bindings = {} of String => Type
      tmpl.params.each_with_index do |p, i|
        at = arg_tys[ i ]?
        unify_type_param( p.type_ann, at, tmpl.type_params, bindings ) if at
      end

      args = [] of Type
      tmpl.type_params.each do |tp|
        bound = bindings[ tp ]?
        if bound.nil? || bound.kind.unknown?
          @bag << Catalog::Sema.cannot_infer_type_param( tp, tmpl.name, expr.loc )
          return Type::UNKNOWN
        end
        args << bound
      end

      finish_generic_call( tmpl.name, args, expr, arg_tys, scope )
    end

    # `identity[Int64]( 42 )` : explicit type arguments on the callee.
    private def infer_explicit_generic_call( callee : Index, name : String, expr : Call, scope : Scope ) : Type
      arg_tys = expr.args.map { |a| infer( a, scope ) }
      args = [] of Type
      ( [ callee.index ] + callee.extra_args ).each do |e|
        t = type_arg_from_expr( e )
        if t.nil?
          @bag << Catalog::Sema.unknown_type_argument( e.is_a?( Ident ) ? e.name : "?", e.loc )
          return Type::UNKNOWN
        end
        args << t
      end
      finish_generic_call( name, args, expr, arg_tys, scope )
    end

    private def finish_generic_call( name : String, args : Array( Type ), expr : Call,
                                     arg_tys : Array( Type ), scope : Scope ) : Type
      mono = @mono
      return Type::UNKNOWN unless mono
      mangled = mono.instantiate_func( name, args, expr.loc )
      return Type::UNKNOWN unless mangled

      # The analyser checks the clone's body later (worklist), but this call
      # site needs its signature now for the return type and argument check.
      unless @sigs[ mangled ]?
        if fn = mono.fresh_funcs.find { |f| f.name == mangled }
          @sigs.collect( fn, @nominals )
        end
      end
      sig = @sigs[ mangled ]?
      return Type::UNKNOWN unless sig

      expr.callee = Ident.new( mangled, expr.callee.loc )
      check_call_args( name, sig.params, expr.args, expr.loc, scope, arg_tys )
      sig.ret
    end

    # A method call on a primitive receiver resolves through the reopened
    # builtin's `TypeInfo` (`struct Int … end` in the Core) : exact width
    # first (`Int32`), then the inferred family (`Int`).
    private def primitive_method( recv_ty : Type, name : String ) : FuncSig?
      if recv_ty.pointer? && (mono = @mono) && mono.type_template?("Pointer")
        mangled = mono.mangle("Pointer", [recv_ty.pointee])
        unless @types.has_key?(mangled) || mono.instantiated?(mangled)
          mono.instantiate_type("Pointer", [recv_ty.pointee], Span.new("<compiler>", 0_u32, 0_u32, 0_u32))
          @collector.try(&.drain_instantiations)
        end
      end

      recv_ty.reopen_names.each do |owner|
        next unless info = @types[ owner ]?
        if sig = info.methods[ name ]?
          return sig
        end
        info.mixins.each do |mixin_name|
          if sig = find_method( mixin_name, name )
            return sig
          end
        end
      end
      nil
    end

    # Walks the superclass chain, then mixins, looking for a method named
    # `name` declared on `type_name`. Inherited fields already live in the
    # subclass's own packed layout (prefix-shared), so field lookup doesn't
    # need this walk : only methods do.
    private def find_method( type_name : String, name : String ) : FuncSig?
      return nil if name == "initialize" || name == "finalize"
      info = @types[ type_name ]?
      return nil unless info
      if sig = info.methods[ name ]?
        return sig
      end
      if sup = info.superclass
        if sig = find_method( sup, name )
          return sig
        end
      end
      info.mixins.each do |m|
        if sig = find_method( m, name )
          return sig
        end
      end
      nil
    end

    # A class with no `initialize` of its own inherits its nearest ancestor's
    # (`NetworkPrinter < Device` : `NetworkPrinter.new("OfficeJet")` must
    # type-check against `Device#initialize`'s params, not fall through to
    # the struct-style "one arg per field" default).
    private def find_initializer_owner_info( type_name : String ) : TypeInfo?
      info = @types[ type_name ]?
      return nil unless info
      return info unless info.initializers.empty?
      info.superclass.try { |s| find_initializer_owner_info( s ) }
    end

    private def find_field( type_name : String, name : String ) : FieldSlot?
      @types[ type_name ]?.try( &.layout ).try( &.field?( name ) )
    end

    private def operator_method( lt : Type, op_name : String ) : FuncSig?
      return nil unless lt.is_a?( NominalType )
      find_method( lt.name, op_name )
    end

    # True for either the legacy `Type::STR` primitive (bare-TypeChecker
    # specs with no Core injected) or the nominal Core `String` class —
    # the only two shapes a "string" type can take post de-hardcoding.
    private def string_ty?( t : Type ) : Bool
      t.kind.str? || ( t.is_a?( NominalType ) && t.name == "String" )
    end

    # `declared` accepts `actual` if they're equal, or `actual` is a subclass
    # of `declared` : walks the superclass chain (`Device` accepts `UsbDrive`).
    private def type_compatible?( declared : Type, actual : Type ) : Bool
      return true if declared.kind.unknown? || actual.kind.unknown?
      return true if declared == actual
      # Legacy `Type::STR` (the builtin `.to_string` fallback for a type with
      # no `to_string` of its own — currently only `Float`) and the nominal
      # Core `String` are runtime-interchangeable (`Vm#to_string_value`
      # builds a real `String` instance either way) : accept either wherever
      # the other is declared.
      return true if string_ty?( declared ) && string_ty?( actual )
      # Class references and pointers are implicitly nilable : `nil` is
      # assignable wherever an Object reference or Pointer is declared.
      return true if actual.nil_type? && ( declared.kind.object? || declared.pointer? )
      # Void* compatibility with any pointer type
      if declared.pointer? && actual.pointer?
        return true if declared.void_pointer? || actual.void_pointer?
      end
      # No implicit String→UInt8* coercion : the Core passes `.data`/`.size`
      # explicitly (`write(1, str.data, str.size)`) — a String no longer
      # silently decays to a raw pointer at a call boundary.
      if declared.is_a?( NominalType ) && actual.is_a?( NominalType )
        cur = actual.name
        while cur
          return true if cur == declared.name
          cur = @types[ cur ]?.try( &.superclass )
        end
      end
      false
    end

    private def infer_binary( expr : BinaryOp, scope : Scope ) : Type
      lt = infer( expr.left, scope )
      rt = infer( expr.right, scope )
      unknown = lt.kind.unknown? || rt.kind.unknown?

      case expr.op

      when .plus?, .minus?, .star?, .slash?, .percent?,
           .amp_plus?, .amp_minus?, .amp_star?, .amp_star_star?,
           .slash_slash?
        # `String + String` dispatches through `operator_method` below (the
        # Core's `String#+`) like any other nominal operator overload — but
        # a bare `TypeChecker` with no Core loaded (unit specs) never sees a
        # `NominalType` "String", only the legacy `Type::STR` primitive, so
        # `operator_method` (requires `NominalType`) can't fire for it :
        # keep that one case as a builtin shortcut.
        if expr.op.plus? && string_ty?( lt ) && string_ty?( rt ) && !lt.is_a?( NominalType )
          return string_type
        end
        if lt.pointer? && ( expr.op.plus? || expr.op.minus? )
          if rt.integer?
            return lt
          else
            @bag << Catalog::Sema.binary_numeric( op_text( expr.op ), lt.to_s, rt.to_s, expr.loc )
            return Type::UNKNOWN
          end
        end
        if ( m = operator_method( lt, op_text( expr.op ) ) )
          unless m.params.size == 1 && ( m.params[ 0 ].kind.unknown? || rt.kind.unknown? || m.params[ 0 ] == rt ||
                 ( string_ty?( m.params[ 0 ] ) && string_ty?( rt ) ) )
            @bag << Catalog::Sema.argument_type( 1, "#{lt}##{op_text( expr.op )}", m.params[ 0 ]?.try( &.to_s ) || "?", rt.to_s, expr.loc )
          end
          return m.ret
        end
        return lt.numeric? ? lt : ( rt.numeric? ? rt : Type::UNKNOWN ) if unknown
        unless lt.numeric? && lt == rt
          @bag << Catalog::Sema.binary_numeric( op_text( expr.op ), lt.to_s, rt.to_s, expr.loc )
          return Type::UNKNOWN
        end
        lt

      when .star_star?
        return lt.numeric? ? lt : ( rt.numeric? ? rt : Type::UNKNOWN ) if unknown
        unless lt.numeric? && lt == rt
          @bag << Catalog::Sema.binary_numeric( op_text( expr.op ), lt.to_s, rt.to_s, expr.loc )
          return Type::UNKNOWN
        end
        lt

      when .lt?, .gt?, .lt_eq?, .gt_eq?, .spaceship?
        return Type::BOOL if unknown
        unless lt.numeric? && lt == rt
          @bag << Catalog::Sema.comparison_numeric( lt.to_s, rt.to_s, expr.loc )
        end
        expr.op.spaceship? ? Type::INT : Type::BOOL

      when .eq_eq?, .bang_eq?, .eq_eq_eq?, .match_op?, .not_match_op?
        if ( expr.op.eq_eq? || expr.op.bang_eq? ) && lt.is_a?( NominalType ) && operator_method( lt, "==" )
          return Type::BOOL
        end
        return Type::BOOL if unknown
        # `typeof(x) == "SomeType"` : `typeof` still yields the legacy
        # `Type::STR` (not a `NominalType`, so the `operator_method` branch
        # above never fires), compared against a real nominal `String`
        # literal — bridge the two `string_ty?` shapes here too.
        if ( expr.op.eq_eq? || expr.op.bang_eq? ) && string_ty?( lt ) && string_ty?( rt )
          return Type::BOOL
        end
        if expr.op.match_op? || expr.op.not_match_op?
          unless string_ty?( lt ) && rt.kind.regex?
            @bag << Catalog::Sema.incomparable( lt.to_s, rt.to_s, expr.loc )
          end
          return Type::BOOL
        end

        if expr.op.eq_eq_eq? && lt.kind.regex? && string_ty?( rt )
          return Type::BOOL
        end

        if lt.numeric? && rt.numeric?
          return Type::BOOL
        end

        # `x == nil` / `x != nil` : always a legitimate question against a
        # class reference (implicitly nilable) — the runtime compares tags.
        if lt.nil_type? || rt.nil_type?
          other = lt.nil_type? ? rt : lt
          unless other.kind.object? || other.pointer? || other.nil_type?
            @bag << Catalog::Sema.incomparable( lt.to_s, rt.to_s, expr.loc )
          end
          return Type::BOOL
        end

        unless lt == rt || ( lt.pointer? && rt.pointer? && ( lt.void_pointer? || rt.void_pointer? ) )
          @bag << Catalog::Sema.incomparable( lt.to_s, rt.to_s, expr.loc )
        end
        Type::BOOL

      when .and?, .or?, .amp?, .pipe?, .caret?, .lt_lt?, .gt_gt?
        if expr.op.amp? || expr.op.pipe? || expr.op.caret? || expr.op.lt_lt? || expr.op.gt_gt?
          unless lt.integer? && rt.integer?
            @bag << Catalog::Sema.binary_numeric( op_text( expr.op ), lt.to_s, rt.to_s, expr.loc )
          end
          return Type::INT
        end
        Type::BOOL
      else
        @bag << Catalog::Sema.unsupported_operator( op_text( expr.op ), expr.loc )
        Type::UNKNOWN
      end
    end

    private def infer_unary( expr : UnaryOp, scope : Scope ) : Type
      ot = infer( expr.operand, scope )

      case expr.op

      when .minus?
        return ot if ot.kind.unknown?
        @bag << Catalog::Sema.unary_numeric( ot.to_s, expr.loc ) unless ot.numeric?
        ot

      when .tilde?
        return ot if ot.kind.unknown?
        unless ot.integer?
          @bag << Catalog::Sema.unary_numeric( ot.to_s, expr.loc )
        end
        ot

      when .bang?
        Type::BOOL

      when .star?
        return ot if ot.kind.unknown?
        if ot.pointer?
          ot.pointee
        else
          @bag << Catalog::Sema.deref_non_pointer( ot.to_s, expr.loc )
          Type::UNKNOWN
        end

      when .amp?
        return ot if ot.kind.unknown?
        if lvalue?( expr.operand )
          Type.pointer( ot )
        else
          @bag << Catalog::Sema.address_of_non_lvalue( expr.loc )
          Type::UNKNOWN
        end

      else
        @bag << Catalog::Sema.unsupported_unary( expr.loc )
        Type::UNKNOWN
      end

    end

    private def infer_call( expr : Call, scope : Scope ) : Type
      callee = expr.callee

      # `identity[Int64]( 42 )` : explicit generic function instantiation.
      if callee.is_a?( Index ) && ( base = callee.receiver ).is_a?( Ident ) &&
         ( mono = @mono ) && mono.func_template?( base.name )
        return infer_explicit_generic_call( callee, base.name, expr, scope )
      end

      unless callee.is_a?( Ident )
        @bag << Catalog::Sema.non_direct_call( expr.loc )
        expr.args.each { |a| infer( a, scope ) }
        return Type::UNKNOWN
      end
      if expr.block
        @bag << Catalog::Sema.block_arg( expr.loc )
      end

      name = callee.name

      # Unqualified call inside a method body first resolves against `self`'s
      # own methods (including inherited/mixed-in ones) before free functions.
      if ( owner = @self_type ) && !scope.local?( name )
        if m = find_method( owner.name, name )
          is_caller_static = @current_method.try( &.is_static ) || false
          if !is_caller_static || m.is_static
            check_call_args( name, m.params, expr.args, expr.loc, scope )
              return m.ret
          end
        elsif owner.kind.mixin?
          # A mixin method calling another mixin's method by name : the
          # concrete resolution depends on whichever class ends up including
          # both, which isn't known here. Accept it leniently (duck-typed).
          expr.args.each { |a| infer( a, scope ) }
          return Type::UNKNOWN
        end
      end

      sig  = @sigs[ name ]?

      # `identity( 42 )` : the callee names a generic function template; the
      # type arguments are inferred by unifying the declared parameter
      # annotations against the actual argument types.
      if sig.nil? && ( mono = @mono ) && ( tmpl = mono.func_template( name ) )
        return infer_generic_call( tmpl, expr, scope )
      end

      if sig.nil?
        @bag << Catalog::Sema.undefined_function( name, expr.loc, Suggest.closest( name, @sigs.names ) )
        expr.args.each { |a| infer( a, scope ) }
        return Type::UNKNOWN
      end

      unless sig.params.size == expr.args.size
        @bag << Catalog::Sema.arity_mismatch( name, sig.params.size, expr.args.size, expr.loc )
      end

      expr.args.each_with_index do |arg, i|
        at    = infer( arg, scope )
        param = sig.params[ i ]?
        next if param.nil?
        next if type_compatible?( param, at )
        @bag << Catalog::Sema.argument_type( i + 1, name, param.to_s, at.to_s, arg.loc )
      end

      sig.ret
    end

    private def infer_if( expr : IfExpr, scope : Scope ) : Type
      infer( expr.cond, scope )
      then_ty = check_body( expr.then_b, scope )
      expr.elsifs.each do |cond, body|
        infer( cond, scope )
        check_body( body, scope )
      end
      if eb = expr.else_b
        else_ty = check_body( eb, scope )
        then_ty
      else
        Type::NIL
      end
    end

    private def infer_while( expr : WhileExpr, scope : Scope ) : Type
      infer( expr.cond, scope )
      @loop_depth += 1
      check_body( expr.body, scope )
      @loop_depth -= 1
      Type::NIL
    end

    #------------------------------------------------------------------------------------

    private def op_text( kind : TokenKind ) : String
      case kind
      when .plus?        then "+"
      when .minus?       then "-"
      when .star?        then "*"
      when .slash?       then "/"
      when .percent?     then "%"
      when .amp_plus?    then "&+"
      when .amp_minus?   then "&-"
      when .amp_star?    then "&*"
      when .amp_star_star? then "&**"
      when .slash_slash? then "//"
      when .star_star?   then "**"
      when .amp?         then "&"
      when .pipe?        then "|"
      when .caret?       then "^"
      when .tilde?       then "~"
      when .lt_lt?       then "<<"
      when .gt_gt?       then ">>"
      when .spaceship?   then "<=>"
      when .eq_eq_eq?    then "==="
      when .match_op?    then "=~"
      when .not_match_op? then "!~"
      else                kind.to_s
      end
    end

    private def infer_pipe( expr : PipeExpr, scope : Scope ) : Type
      left = expr.left
      right = expr.right
      desugared : AExpr

      case right
      when Ident
        desugared = Call.new( right, [ left.as( AExpr ) ] of AExpr, nil, right.loc )
      when Call
        desugared = Call.new( right.callee, Array( AExpr ){ left.as( AExpr ) } + right.args, right.block, right.loc )
      when MemberAccess
        desugared = MethodCall.new( right.receiver, right.name, [ left.as( AExpr ) ] of AExpr, nil, right.safe, right.loc )
      when MethodCall
        desugared = MethodCall.new( right.receiver, right.name, Array( AExpr ){ left.as( AExpr ) } + right.args, right.block, right.safe, right.loc )
      else
        @bag << Catalog::Sema.invalid_pipeline_target( right.loc )
        return Type::UNKNOWN
      end

      resolved_ty = infer( desugared, scope )
      expr.desugared = desugared
      right.resolved_type = resolved_ty
      expr.resolved_type = resolved_ty
      resolved_ty
    end

    private def type_name( node : ANode ) : String
      node.class.name.split( "::" ).last
    end

    private def lvalue?( node : AExpr ) : Bool
      node.is_a?( Ident ) || node.is_a?( InstanceVar ) || node.is_a?( ClassVar ) || node.is_a?( MemberAccess ) ||
        ( node.is_a?( UnaryOp ) && node.op.star? )
    end

    private def local_address_taken?( expr : AExpr, scope : Scope ) : Bool
      if expr.is_a?( UnaryOp ) && expr.op.amp?
        target = expr.operand
        if target.is_a?( Ident )
          return !scope.lookup( target.name ).nil?
        end
      end
      false
    end
  end


end
