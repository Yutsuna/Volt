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


  class ArrayLit < AExpr
    property elements : Array( AExpr )

    def initialize( @elements : Array( AExpr ), loc : Span )
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

    def initialize( @target : AExpr, @type_ann : ATypeNode?, @value : AExpr, loc : Span )
      super( loc )
    end
  end


  # forward declaration — BlockExpr is referenced by Call / MethodCall
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

    def initialize( @receiver : AExpr, @name : String, @args : Array( AExpr ),
                    @block : BlockExpr?, @safe : Bool, loc : Span )
      super( loc )
    end
  end


  # receiver[ index ]
  class Index < AExpr
    property receiver : AExpr
    property index    : AExpr

    def initialize( @receiver : AExpr, @index : AExpr, loc : Span )
      super( loc )
    end
  end


  # left |> right
  class PipeExpr < AExpr
    property left  : AExpr
    property right : AExpr

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


end
