module Volt::Frontend


  class IntLit < AExpr
    property value : Int64

    def initialize( @value : Int64, loc : Span )
      super( loc )
    end
  end


  class FloatLit < AExpr
    property value : Float64

    def initialize( @value : Float64, loc : Span )
      super( loc )
    end
  end


  class StringLit < AExpr
    property value : String

    def initialize( @value : String, loc : Span )
      super( loc )
    end
  end


  class BoolLit < AExpr
    property value : Bool

    def initialize( @value : Bool, loc : Span )
      super( loc )
    end
  end


  class NilLit < AExpr
    def initialize( loc : Span )
      super( loc )
    end
  end


  # [ a, b, c ]  |  [ a, b, c ] of Elem — a fixed-size *stack* array literal
  # (`Elem[N]`, same register-run representation as `buf : UInt8[ 20 ]`).
  # `elem_ann` carries the optional `of` element-type annotation.
  class ArrayLit < AExpr
    property elements : Array( AExpr )
    property elem_ann : ATypeNode?

    def initialize( @elements : Array( AExpr ), loc : Span, @elem_ann : ATypeNode? = nil )
      super( loc )
    end
  end


  # { key => value, ... }  |  { name: value, ... }  |  {} of K => V
  # `key_ann`/`val_ann` carry the optional `of K => V` annotation (the only
  # way an *empty* literal can name its types).
  class HashLiteralExpr < AExpr
    property pairs : Array( { AExpr, AExpr } )
    property key_ann : ATypeNode?
    property val_ann : ATypeNode?

    def initialize( @pairs : Array( { AExpr, AExpr } ), loc : Span,
                    @key_ann : ATypeNode? = nil, @val_ann : ATypeNode? = nil )
      super( loc )
    end
  end


  class Ident < AExpr
    property name : String

    def initialize( @name : String, loc : Span )
      super( loc )
    end
  end


  class SelfExpr < AExpr
    def initialize( loc : Span )
      super( loc )
    end
  end


  # super( args ) | super — calls the nearest ancestor's implementation of the
  # enclosing method. A bare `super` (implicit_args) forwards the enclosing
  # method's parameters; Semantic materialises them into `args`.
  class SuperCall < AExpr
    property args          : Array( AExpr )
    property implicit_args : Bool

    def initialize( @args : Array( AExpr ), @implicit_args : Bool, loc : Span )
      super( loc )
    end
  end


  # @name instance-variable reference (valid only inside instance methods)
  class InstanceVar < AExpr
    property name : String

    def initialize( @name : String, loc : Span )
      super( loc )
    end
  end


  # @@name module/class-variable reference (process-global storage)
  class ClassVar < AExpr
    property name : String

    def initialize( @name : String, loc : Span )
      super( loc )
    end
  end


  # receiver.name field read without arguments; the semantic pass reclassifies
  # it as a zero-arg method call when `name` resolves to a method
  class MemberAccess < AExpr
    property receiver : AExpr
    property name     : String
    property safe     : Bool           # true for ?.  navigation

    def initialize( @receiver : AExpr, @name : String, @safe : Bool, loc : Span )
      super( loc )
    end
  end


  class BinaryOp < AExpr
    property left  : AExpr
    property op    : TokenKind
    property right : AExpr

    def initialize( @left : AExpr, @op : TokenKind, @right : AExpr, loc : Span )
      super( loc )
    end
  end


  class UnaryOp < AExpr
    property op      : TokenKind
    property operand : AExpr

    def initialize( @op : TokenKind, @operand : AExpr, loc : Span )
      super( loc )
    end
  end


  # name = value  |  name : Type = value  |  obj.field = value
  class Assign < AExpr
    property target   : AExpr
    property type_ann : ATypeNode?
    property value    : AExpr
    # Set by the semantic pass when `target` is a `MemberAccess` with no
    # matching field : `obj.name = v` then dispatches to a `name=` setter
    # method instead of a `STORE_FIELD` (see `TypeChecker#infer_assign_member`).
    property is_setter_call : Bool = false

    def initialize( @target : AExpr, @type_ann : ATypeNode?, @value : AExpr, loc : Span )
      super( loc )
    end
  end


  # name : Type   (no initializer) : reserves a fresh, zero-initialized local
  # of the annotated type — the one shape `Assign` can't express since it
  # requires a value. In practice this is how a fixed-size stack array is
  # declared (`buf : UInt8[ 20 ]`) : there is no single element value to
  # initialize it *with*, only a size to reserve.
  class VarDecl < AExpr
    property name     : String
    property type_ann : ATypeNode

    def initialize( @name : String, @type_ann : ATypeNode, loc : Span )
      super( loc )
    end
  end


  # forward declaration BlockExpr is referenced by Call / MethodCall
  class BlockExpr < AExpr
    property params : Array( String )
    property body   : Array( ANode )

    def initialize( @params : Array( String ), @body : Array( ANode ), loc : Span )
      super( loc )
    end
  end


  # free function call:  callee( args ) { block }
  class Call < AExpr
    property callee : AExpr
    property args   : Array( AExpr )
    property block  : BlockExpr?

    def initialize( @callee : AExpr, @args : Array( AExpr ), @block : BlockExpr?, loc : Span )
      super( loc )
    end
  end


  # receiver.name( args ) { block }   |   receiver?.name( args )
  class MethodCall < AExpr
    property receiver : AExpr
    property name     : String
    property args     : Array( AExpr )
    property block    : BlockExpr?
    property safe     : Bool           # true for ?.  navigation
    # Set by `TypeChecker` for `.is_a?`/`.has?` : both are pure compile-time
    # reflection, resolved to a constant `true`/`false` during typecheck
    # (never dispatched as a real method) — `FunctionEmiter` reads this
    # instead of compiling a call when it's non-nil.
    property resolved_bool : Bool?

    def initialize( @receiver : AExpr, @name : String, @args : Array( AExpr ),
                    @block : BlockExpr?, @safe : Bool, loc : Span )
      super( loc )
    end
  end


  # receiver[ index ]  |  receiver[ index, extra... ]
  # The multi-argument form only occurs as a generic type reference
  # (`Pair[String, Int64]`) : the semantic pass interprets an `Index` whose
  # receiver names a generic template as an explicit instantiation.
  class Index < AExpr
    property receiver   : AExpr
    property index      : AExpr
    property extra_args : Array( AExpr ) = [] of AExpr

    def initialize( @receiver : AExpr, @index : AExpr, loc : Span )
      super( loc )
    end
  end


  # left |> right
  class PipeExpr < AExpr
    property left  : AExpr
    property right : AExpr
    property desugared : AExpr? = nil

    def initialize( @left : AExpr, @right : AExpr, loc : Span )
      super( loc )
    end
  end


  # if cond \n then_b \n (elsif cond \n body)* \n else else_b \n end
  class IfExpr < AExpr
    property cond    : AExpr
    property then_b  : Array( ANode )
    property elsifs  : Array( { AExpr, Array( ANode ) } )
    property else_b  : Array( ANode )?

    def initialize( @cond : AExpr, @then_b : Array( ANode ),
                    @elsifs : Array( { AExpr, Array( ANode ) } ),
                    @else_b : Array( ANode )?, loc : Span )
      super( loc )
    end
  end


  class MatchArm
    property patterns : Array( AExpr )
    property body     : AExpr
    property is_else  : Bool

    def initialize( @patterns : Array( AExpr ), @body : AExpr, @is_else : Bool )
    end
  end


  # match value \n when ... \n else ... \n end
  class MatchExpr < AExpr
    property value : AExpr
    property arms  : Array( MatchArm )

    def initialize( @value : AExpr, @arms : Array( MatchArm ), loc : Span )
      super( loc )
    end
  end


  # from..to  |  from...to (exclusive)
  class RangeExpr < AExpr
    property from      : AExpr
    property to        : AExpr
    property exclusive : Bool

    def initialize( @from : AExpr, @to : AExpr, @exclusive : Bool, loc : Span )
      super( loc )
    end
  end


  class AwaitExpr < AExpr
    property expr : AExpr

    def initialize( @expr : AExpr, loc : Span )
      super( loc )
    end
  end


  class ReturnExpr < AExpr
    property value : AExpr?

    def initialize( @value : AExpr?, loc : Span )
      super( loc )
    end
  end


  class BreakExpr < AExpr
    property value : AExpr?

    def initialize( @value : AExpr?, loc : Span )
      super( loc )
    end
  end


  class NextExpr < AExpr
    property value : AExpr?

    def initialize( @value : AExpr?, loc : Span )
      super( loc )
    end
  end


  class RaiseExpr < AExpr
    property value : AExpr

    def initialize( @value : AExpr, loc : Span )
      super( loc )
    end
  end


  # while cond \n body \n end
  class WhileExpr < AExpr
    property cond : AExpr
    property body : Array( ANode )

    def initialize( @cond : AExpr, @body : Array( ANode ), loc : Span )
      super( loc )
    end
  end


  class RegexLit < AExpr
    property value : String

    def initialize( @value : String, loc : Span )
      super( loc )
    end
  end


  # `:name` — an interned identifier. As the argument of `.has?` it stays a
  # compile-time-only token (folded by `TypeChecker`) ; everywhere else it is
  # a first-class runtime value of type `Symbol` : its interned `Int64` id
  # (`Frontend::Symbols`), which makes symbol comparison and hashing integer
  # operations.
  class SymbolLit < AExpr
    property value : String

    def initialize( @value : String, loc : Span )
      super( loc )
    end
  end


  class TypeofExpr < AExpr
    property operand : AExpr
    property resolved_operand_type : Type?

    def initialize( @operand : AExpr, loc : Span )
      super( loc )
    end
  end


  class SizeofExpr < AExpr
      property type_node : ATypeNode
      property byte_size : Int32 = 0

      def initialize( @type_node : ATypeNode, loc : Span )
        super( loc )
      end
    end


end
