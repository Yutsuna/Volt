module Volt::Frontend


  enum TypeKind
    Int8
    Int16
    Int32
    Int       # inferred integer type
    Float     # inferred floating-point type
    Float32
    Float64
    Bool
    Str
    Regex
    Nil
    Func
    Object     # instance of a user-defined `class`  (heap reference)
    Struct     # value of a user-defined `struct`     (by-value block)
    Unknown
  end


end
