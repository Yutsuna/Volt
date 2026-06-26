module Volt::Frontend


  class FuncSig
    property name      : String
    property params    : Array( Type )
    property ret       : Type
    property extern    : Bool
    property lib       : String?
    property decl_span : Span?

    def initialize( @name, @params, @ret, @extern = false, @lib = nil, @decl_span = nil )
    end
  end


  class TypedProgram
    property program    : Program
    property functions  : Array( FuncDecl )
    property top_level  : Array( ANode )
    property signatures : Hash( String, FuncSig )

    def initialize( @program, @functions, @top_level, @signatures )
    end
  end


end
