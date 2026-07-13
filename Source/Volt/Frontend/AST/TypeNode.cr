module Volt::Frontend


  class SimpleType < ATypeNode
    property name : String

    def initialize( @name : String, loc : Span )
      super( loc )
    end
  end


  # Box[T], Array[String], Hash[K, V]
  class GenericType < ATypeNode
    property name   : String
    property params : Array( ATypeNode )

    @params : Array( ATypeNode )

    def initialize( @name : String, @params : Array( ATypeNode ), loc : Span )
      super( loc )
    end
  end


  # T -> U  (used in block / lambda type annotations)
  class FuncType < ATypeNode
    property params      : Array( ATypeNode )
    property return_type : ATypeNode

    @params : Array( ATypeNode )

    def initialize( @params : Array( ATypeNode ), @return_type : ATypeNode, loc : Span )
      super( loc )
    end
  end


  # SomeType?
  class NilableType < ATypeNode
    property inner : ATypeNode

    def initialize( @inner : ATypeNode, loc : Span )
      super( loc )
    end
  end


  # *T
  class PointerType < ATypeNode
    property inner : ATypeNode

    def initialize( @inner : ATypeNode, loc : Span )
      super( loc )
    end
  end


  # Elem[N] : a fixed-size stack array of `N` `Elem`s (`Int64[5]`, `UInt8[20]`),
  # e.g. C++'s `std::array<Elem, N>`. `size` is the compile-time integer
  # literal parsed straight out of the brackets — never a runtime value.
  class ArrayType < ATypeNode
    property elem : ATypeNode
    property size : Int32

    def initialize( @elem : ATypeNode, @size : Int32, loc : Span )
      super( loc )
    end
  end


end

