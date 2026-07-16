module Volt::Frontend


  enum TypeKind
    Int8
    Int16
    Int32
    Int64
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
    UInt8
    UInt16
    UInt32
    UInt64
    UInt
    Pointer
    Array     # fixed-size stack array (`Elem[N]`, e.g. `Int64[5]`)
    Symbol    # interned identifier (`:name`) — an Int64 id at runtime
    Unknown
  end


end
