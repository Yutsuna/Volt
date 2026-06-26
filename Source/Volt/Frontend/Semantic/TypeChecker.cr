module Volt::Frontend


  class TypeChecker
    def initialize( @sigs : SignatureTable, @bag : DiagnosticBag )
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

    #------------------------------------------------------------------------------------

    # Returns the value type of the last expression, for implicit return.
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
      when Ident      then infer_ident( expr, scope )
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
      when MethodCall
        if expr.name == "includes?" && expr.receiver.is_a?( RangeExpr )
          infer( expr.receiver, scope )
          expr.args.each { |a| infer( a, scope ) }
          return Type::BOOL
        end
        @bag << Catalog::Sema.unsupported_expr( "method call `#{expr.name}`", expr.loc )
        Type::UNKNOWN
      when ReturnExpr
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
      @bag << Catalog::Sema.undefined_variable( expr.name, expr.loc, Suggest.closest( expr.name, scope.visible_names ) )
      Type::UNKNOWN
    end

    private def infer_assign( expr : Assign, scope : Scope ) : Type
      target = expr.target
      unless target.is_a?( Ident )
        @bag << Catalog::Sema.non_simple_assign( expr.loc )
        infer( expr.value, scope )
        return Type::UNKNOWN
      end

      name      = target.name
      value_ty  = infer( expr.value, scope )

      if ann = expr.type_ann
        declared = Type.from_annotation( ann )
        if declared.nil?
          @bag << Catalog::Sema.unsupported_annotation( ann.loc )
        elsif !declared.kind.unknown? && !value_ty.kind.unknown? && declared != value_ty
          @bag << Catalog::Sema.annotation_mismatch( name, declared.to_s, value_ty.to_s, expr.loc )
        end
      end

      if existing = scope.lookup( name )
        if !existing.kind.unknown? && !value_ty.kind.unknown? && existing != value_ty
          @bag << Catalog::Sema.reassign_type( name, existing.to_s, value_ty.to_s, expr.loc )
        end
      end

      scope.define( name, value_ty )
      target.resolved_type = value_ty
      value_ty
    end

    private def infer_binary( expr : BinaryOp, scope : Scope ) : Type
      lt = infer( expr.left, scope )
      rt = infer( expr.right, scope )
      unknown = lt.kind.unknown? || rt.kind.unknown?

      case expr.op
      when .plus?, .minus?, .star?, .slash?, .percent?,
           .amp_plus?, .amp_minus?, .amp_star?, .amp_star_star?,
           .slash_slash?
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
        return Type::BOOL if unknown
        if expr.op.match_op? || expr.op.not_match_op?
          unless lt.kind.str? && rt.kind.regex?
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
        if !param.kind.unknown? && !at.kind.unknown? && param != at
          @bag << Catalog::Sema.argument_type( i + 1, name, param.to_s, at.to_s, arg.loc )
        end
      end

      sig.ret
    end

    private def infer_if( expr : IfExpr, scope : Scope ) : Type
      infer( expr.cond, scope )
      check_body( expr.then_b, scope )
      expr.elsifs.each do |cond, body|
        infer( cond, scope )
        check_body( body, scope )
      end
      if eb = expr.else_b
        check_body( eb, scope )
      end
      Type::NIL
    end

    private def infer_while( expr : WhileExpr, scope : Scope ) : Type
      infer( expr.cond, scope )
      check_body( expr.body, scope )
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
