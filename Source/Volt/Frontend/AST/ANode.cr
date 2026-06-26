module Volt::Frontend


  abstract class ANode
    property loc : Span

    def initialize( @loc : Span )
    end
  end


  abstract class AExpr < ANode
    property resolved_type : Type? = nil
  end


  abstract class ADecl < ANode
  end


  abstract class ATypeNode < ANode
  end


end
