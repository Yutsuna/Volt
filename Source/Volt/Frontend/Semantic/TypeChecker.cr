module Volt::Frontend


  class TypeChecker
    def initialize( @sigs : SignatureTable, @bag : DiagnosticBag,
                    @types : Hash( String, TypeInfo ) = {} of String => TypeInfo,
                    @nominals : Hash( String, NominalType ) = {} of String => NominalType )
      @self_type  = nil.as( TypeInfo? )
      @loop_depth = 0
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

      previous  = @self_type
      @self_type = info
      check_body( fn.body, scope )
      @self_type = previous
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
      when StringLit  then Type::STR
      when BoolLit    then Type::BOOL
      when NilLit     then Type::NIL
      when RegexLit   then Type::REGEX
      when Ident        then infer_ident( expr, scope )
      when InstanceVar  then infer_instance_var( expr, scope )
      when ClassVar     then infer_class_var( expr, scope )
      when MemberAccess then infer_member_access( expr, scope )
      when SelfExpr     then infer_self( expr, scope )
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
      when ReturnExpr
        if v = expr.value
          infer( v, scope )
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
      if ( owner = @self_type ) && !scope.local?( expr.name ) && ( m = find_method( owner.name, expr.name ) )
        @bag << Catalog::Sema.arity_mismatch( expr.name, m.params.size, 0, expr.loc ) unless m.params.empty?
        return m.ret
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

    private def infer_assign( expr : Assign, scope : Scope ) : Type
      case target = expr.target
      when Ident        then infer_assign_ident( expr, target, scope )
      when InstanceVar  then infer_assign_ivar( expr, target, scope )
      when ClassVar     then infer_assign_class_var( expr, target, scope )
      when MemberAccess then infer_assign_member( expr, target, scope )
      else
        @bag << Catalog::Sema.non_simple_assign( expr.loc )
        infer( expr.value, scope )
        Type::UNKNOWN
      end
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
      end

      scope.define( name, static_ty )
      target.resolved_type = static_ty
      static_ty
    end

    private def infer_assign_ivar( expr : Assign, target : InstanceVar, scope : Scope ) : Type
      value_ty = infer( expr.value, scope )
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
      return Type::UNKNOWN unless recv_ty.is_a?( NominalType )

      field = find_field( recv_ty.name, target.name )
      if field.nil?
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
      if owner = @self_type
        @nominals[ owner.name ]? || Type::UNKNOWN
      else
        @bag << Catalog::Sema.self_outside_method( expr.loc )
        Type::UNKNOWN
      end
    end

    private def infer_member_access( expr : MemberAccess, scope : Scope ) : Type

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
      # `.to_s` is the one builtin every value supports ahead of a fuller
      # builtin method table.
      unless recv_ty.is_a?( NominalType )
        return Type::STR if expr.name == "to_s"
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

      return Type::STR if expr.name == "to_s"

      @bag << Catalog::Sema.unknown_field_or_method( recv_ty.name, expr.name, expr.loc )
      Type::UNKNOWN
    end

    private def infer_method_call( expr : MethodCall, scope : Scope ) : Type
      if expr.name == "includes?" && expr.receiver.is_a?( RangeExpr )
        infer( expr.receiver, scope )
        expr.args.each { |a| infer( a, scope ) }
        return Type::BOOL
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

      # Minimal builtin: `.to_s` is usable on any value (the samples rely on
      # it for string-concatenation logging) ahead of a fuller builtin table.
      if expr.name == "to_s" && expr.args.empty?
        return Type::STR
      end

      unless recv_ty.is_a?( NominalType )
        @bag << Catalog::Sema.unsupported_expr( "method call `.#{expr.name}` on non-object type `#{recv_ty}`", expr.loc )
        expr.args.each { |a| infer( a, scope ) }
        return Type::UNKNOWN
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

    # `Module.method( args )` : a module is a static namespace, so every one of
    # its methods is callable on the module itself (there are no instances). A
    # `private` static method is reachable only from within the same module.
    private def infer_static_member( info : TypeInfo, name : String, args : Array( AExpr ), loc : Span, scope : Scope ) : Type
      sig = info.methods[ name ]?
      if sig.nil?
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

    private def infer_constructor_call( info : TypeInfo, expr : MethodCall, scope : Scope ) : Type
      @bag << Catalog::Sema.abstract_instantiation( info.name, expr.loc ) if info.is_abstract

      if init = find_initializer( info.name )
        check_call_args( "#{info.name}.new", init.params, expr.args, expr.loc, scope )
      elsif layout = info.layout
        check_call_args( "#{info.name}.new", layout.fields.map( &.type ), expr.args, expr.loc, scope )
      else
        expr.args.each { |a| infer( a, scope ) }
      end

      @nominals[ info.name ]? || Type::UNKNOWN
    end

    private def check_call_args( name : String, params : Array( Type ), args : Array( AExpr ), loc : Span, scope : Scope ) : Nil
      unless params.size == args.size
        @bag << Catalog::Sema.arity_mismatch( name, params.size, args.size, loc )
      end
      args.each_with_index do |arg, i|
        at    = infer( arg, scope )
        param = params[ i ]?
        next if param.nil?
        next if type_compatible?( param, at )
        @bag << Catalog::Sema.argument_type( i + 1, name, param.to_s, at.to_s, arg.loc )
      end
    end

    # Walks the superclass chain, then mixins, looking for a method named
    # `name` declared on `type_name`. Inherited fields already live in the
    # subclass's own packed layout (prefix-shared), so field lookup doesn't
    # need this walk : only methods do.
    private def find_method( type_name : String, name : String ) : FuncSig?
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
    private def find_initializer( type_name : String ) : FuncSig?
      info = @types[ type_name ]?
      return nil unless info
      info.initializer || info.superclass.try { |s| find_initializer( s ) }
    end

    private def find_field( type_name : String, name : String ) : FieldSlot?
      @types[ type_name ]?.try( &.layout ).try( &.field?( name ) )
    end

    private def operator_method( lt : Type, op_name : String ) : FuncSig?
      return nil unless lt.is_a?( NominalType )
      find_method( lt.name, op_name )
    end

    # `declared` accepts `actual` if they're equal, or `actual` is a subclass
    # of `declared` : walks the superclass chain (`Device` accepts `UsbDrive`).
    private def type_compatible?( declared : Type, actual : Type ) : Bool
      return true if declared.kind.unknown? || actual.kind.unknown?
      return true if declared == actual
      # Class references are implicitly nilable (Java-style) : `nil` is
      # assignable wherever an Object reference is declared. `T?` annotations
      # erase to `T` (see `Type.from_annotation`), so this single rule covers
      # both spellings. Value types (Int/Float/Bool/struct) stay non-nilable.
      return true if actual.nil_type? && declared.kind.object?
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
        return Type::STR if expr.op.plus? && lt.kind.str? && rt.kind.str?
        if ( m = operator_method( lt, op_text( expr.op ) ) )
          unless m.params.size == 1 && ( m.params[ 0 ].kind.unknown? || rt.kind.unknown? || m.params[ 0 ] == rt )
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
        if expr.op.match_op? || expr.op.not_match_op?
          unless lt.kind.str? && rt.kind.regex?
            @bag << Catalog::Sema.incomparable( lt.to_s, rt.to_s, expr.loc )
          end
          return Type::BOOL
        end

        if expr.op.eq_eq_eq? && lt.kind.regex? && rt.kind.str?
          return Type::BOOL
        end

        if lt.numeric? && rt.numeric?
          return Type::BOOL
        end

        # `x == nil` / `x != nil` : always a legitimate question against a
        # class reference (implicitly nilable) — the runtime compares tags.
        if lt.nil_type? || rt.nil_type?
          other = lt.nil_type? ? rt : lt
          unless other.kind.object? || other.nil_type?
            @bag << Catalog::Sema.incomparable( lt.to_s, rt.to_s, expr.loc )
          end
          return Type::BOOL
        end

        unless lt == rt
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

      else
        @bag << Catalog::Sema.unsupported_unary( expr.loc )
        Type::UNKNOWN
      end

    end

    private def infer_call( expr : Call, scope : Scope ) : Type
      callee = expr.callee
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
          check_call_args( name, m.params, expr.args, expr.loc, scope )
          return m.ret
        elsif owner.kind.mixin?
          # A mixin method calling another mixin's method by name : the
          # concrete resolution depends on whichever class ends up including
          # both, which isn't known here. Accept it leniently (duck-typed).
          expr.args.each { |a| infer( a, scope ) }
          return Type::UNKNOWN
        end
      end

      sig  = @sigs[ name ]?
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

    private def type_name( node : ANode ) : String
      node.class.name.split( "::" ).last
    end
  end


end
