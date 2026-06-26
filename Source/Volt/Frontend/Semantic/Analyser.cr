module Volt::Frontend


  class SemanticError < Exception
    getter span : Span

    def initialize( message : ::String, @span : Span )
      super( message )
    end
  end


  # Signature of a callable: a user function (with a chunk index assigned later by the
  # compiler) or a native builtin (with a fixed native id).
  class FuncSig
    property name      : String
    property params    : Array( Type )
    property ret       : Type
    property native_id : Int32?
    property variadic  : Bool

    def initialize( @name, @params, @ret, @native_id = nil, @variadic = false )
    end

    def native? : Bool
      !@native_id.nil?
    end
  end


  # The contract handed to the engine: the parsed program with every AExpr annotated
  # with a resolved type, plus the resolved signature table.
  class TypedProgram
    property program     : Program
    property functions   : Array( FuncDecl )
    property top_level    : Array( ANode )
    property signatures   : Hash( String, FuncSig )

    def initialize( @program, @functions, @top_level, @signatures )
    end
  end


  # Native builtins available to every program. Ids must match Runtime.call_native.
  NATIVE_BUILTINS = {
    "puts"  => 0,
    "print" => 1,
  }


  class Analyser
    @signatures : Hash( String, FuncSig )
    @functions  : Array( FuncDecl )
    @top_level  : Array( ANode )

    def initialize( @program : Program )
      @signatures = {} of String => FuncSig
      @functions  = [] of FuncDecl
      @top_level  = [] of ANode
    end

    def analyse : TypedProgram
      register_natives
      partition_and_collect
      # Phase B: type-check each function body, then the synthetic top-level.
      @functions.each { |fn| check_function( fn ) }
      check_top_level
      TypedProgram.new( @program, @functions, @top_level, @signatures )
    end

    # ---------------------------------------------------------------- phase A

    private def register_natives
      NATIVE_BUILTINS.each do |name, id|
        @signatures[ name ] = FuncSig.new( name, [ Type::UNKNOWN ], Type::NIL,
                                           native_id: id, variadic: true )
      end
    end

    private def partition_and_collect
      @program.nodes.each do |node|
        case node
        when FuncDecl
          collect_signature( node )
          @functions << node
        when AExpr
          @top_level << node
        else
          reject( node, "top-level `#{node.class.name.split("::").last}` is not supported in v1" )
        end
      end
    end

    private def collect_signature( fn : FuncDecl )
      if @signatures.has_key?( fn.name )
        reject( fn, "duplicate definition of `#{fn.name}`" )
      end
      params = fn.params.map do |p|
        ann = p.type_ann
        if ann.nil?
          reject( fn, "parameter `#{p.name}` of `#{fn.name}` needs a type annotation in v1" )
        end
        ty = Type.from_annotation( ann )
        reject( fn, "unsupported parameter type for `#{p.name}` in v1" ) if ty.nil?
        ty
      end
      ret = Type::UNKNOWN
      if rt = fn.return_type
        resolved = Type.from_annotation( rt )
        reject( fn, "unsupported return type for `#{fn.name}` in v1" ) if resolved.nil?
        ret = resolved.not_nil!
      end
      @signatures[ fn.name ] = FuncSig.new( fn.name, params, ret )
    end

    # ---------------------------------------------------------------- phase B

    private def check_function( fn : FuncDecl )
      scope = {} of String => Type
      sig   = @signatures[ fn.name ]
      fn.params.each_with_index { |p, i| scope[ p.name ] = sig.params[ i ] }
      body_ty = check_body( fn.body, scope )
      # Infer return type when it was not annotated.
      sig.ret = body_ty if sig.ret.kind.unknown?
    end

    private def check_top_level
      scope = {} of String => Type
      check_body( @top_level, scope )
    end

    # Returns the value type of the body (its last expression), for implicit return.
    private def check_body( nodes : Array( ANode ), scope : Hash( String, Type ) ) : Type
      last = Type::NIL
      nodes.each do |node|
        unless node.is_a?( AExpr )
          reject( node, "nested `#{node.class.name.split("::").last}` is not supported in v1" )
        end
        last = infer( node.as( AExpr ), scope )
      end
      last
    end

    # ---------------------------------------------------------------- inference

    private def infer( expr : AExpr, scope : Hash( String, Type ) ) : Type
      ty = infer_raw( expr, scope )
      expr.resolved_type = ty
      ty
    end

    private def infer_raw( expr : AExpr, scope : Hash( String, Type ) ) : Type
      case expr
      when IntLit    then Type::INT
      when FloatLit  then Type::FLOAT
      when StringLit then Type::STR
      when BoolLit   then Type::BOOL
      when NilLit    then Type::NIL
      when Ident     then infer_ident( expr, scope )
      when Assign    then infer_assign( expr, scope )
      when BinaryOp  then infer_binary( expr, scope )
      when UnaryOp   then infer_unary( expr, scope )
      when Call      then infer_call( expr, scope )
      when IfExpr    then infer_if( expr, scope )
      when WhileExpr then infer_while( expr, scope )
      when ReturnExpr
        if v = expr.value
          infer( v, scope )
        end
        Type::NIL
      else
        reject( expr, "`#{expr.class.name.split("::").last}` is not supported in v1" )
      end
    end

    private def infer_ident( expr : Ident, scope : Hash( String, Type ) ) : Type
      ty = scope[ expr.name ]?
      reject( expr, "undefined variable `#{expr.name}`" ) if ty.nil?
      ty.not_nil!
    end

    private def infer_assign( expr : Assign, scope : Hash( String, Type ) ) : Type
      target = expr.target
      unless target.is_a?( Ident )
        reject( expr, "only simple `name = value` assignment is supported in v1" )
      end
      value_ty = infer( expr.value, scope )
      if ann = expr.type_ann
        declared = Type.from_annotation( ann )
        reject( expr, "unsupported type annotation in v1" ) if declared.nil?
        unless declared.not_nil! == value_ty
          reject( expr, "type mismatch: `#{target.as( Ident ).name}` declared #{declared} but value is #{value_ty}" )
        end
      end
      name = target.as( Ident ).name
      if existing = scope[ name ]?
        unless existing == value_ty
          reject( expr, "cannot reassign `#{name}` (#{existing}) with a #{value_ty}" )
        end
      end
      scope[ name ] = value_ty
      target.as( Ident ).resolved_type = value_ty
      value_ty
    end

    private def infer_binary( expr : BinaryOp, scope : Hash( String, Type ) ) : Type
      lt = infer( expr.left, scope )
      rt = infer( expr.right, scope )
      case expr.op
      when .plus?, .minus?, .star?, .slash?, .percent?
        unless lt.numeric? && lt == rt
          reject( expr, "operator `#{op_text( expr.op )}` needs matching numeric operands, got #{lt} and #{rt}" )
        end
        lt
      when .lt?, .gt?, .lt_eq?, .gt_eq?
        unless lt.numeric? && lt == rt
          reject( expr, "comparison needs matching numeric operands, got #{lt} and #{rt}" )
        end
        Type::BOOL
      when .eq_eq?, .bang_eq?
        unless lt == rt
          reject( expr, "cannot compare #{lt} with #{rt}" )
        end
        Type::BOOL
      when .and?, .or?
        Type::BOOL
      else
        reject( expr, "operator `#{op_text( expr.op )}` is not supported in v1" )
      end
    end

    private def infer_unary( expr : UnaryOp, scope : Hash( String, Type ) ) : Type
      ot = infer( expr.operand, scope )
      case expr.op
      when .minus?
        reject( expr, "unary `-` needs a numeric operand, got #{ot}" ) unless ot.numeric?
        ot
      when .bang?
        Type::BOOL
      else
        reject( expr, "unary operator is not supported in v1" )
      end
    end

    private def infer_call( expr : Call, scope : Hash( String, Type ) ) : Type
      callee = expr.callee
      unless callee.is_a?( Ident )
        reject( expr, "only direct function calls `name(...)` are supported in v1" )
      end
      if expr.block
        reject( expr, "block arguments are not supported in v1" )
      end
      name = callee.as( Ident ).name
      sig  = @signatures[ name ]?
      reject( expr, "call to undefined function `#{name}`" ) if sig.nil?
      sig = sig.not_nil!
      unless sig.variadic || sig.params.size == expr.args.size
        reject( expr, "`#{name}` expects #{sig.params.size} argument(s), got #{expr.args.size}" )
      end
      expr.args.each_with_index do |arg, i|
        at = infer( arg, scope )
        if !sig.variadic && !sig.params[ i ].kind.unknown? && sig.params[ i ] != at
          reject( arg, "argument #{i + 1} of `#{name}` expects #{sig.params[ i ]}, got #{at}" )
        end
      end
      sig.ret
    end

    private def infer_if( expr : IfExpr, scope : Hash( String, Type ) ) : Type
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

    private def infer_while( expr : WhileExpr, scope : Hash( String, Type ) ) : Type
      infer( expr.cond, scope )
      check_body( expr.body, scope )
      Type::NIL
    end

    # ---------------------------------------------------------------- helpers

    private def op_text( kind : TokenKind ) : String
      case kind
      when .plus?    then "+"
      when .minus?   then "-"
      when .star?    then "*"
      when .slash?   then "/"
      when .percent? then "%"
      else                kind.to_s
      end
    end

    private def reject( node : ANode, msg : String ) : NoReturn
      raise SemanticError.new( msg, node.loc )
    end
  end


end
