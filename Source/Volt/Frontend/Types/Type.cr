module Volt::Frontend


  class Type
    property kind       : TypeKind
    property params     : Array( Type )
    property ret        : Type?
    # Only meaningful when `kind.array?` : the compile-time-known element
    # count of a fixed-size stack array (`Elem[N]`). Unlike `Pointer`, an
    # array's size is part of its *type* (like C++'s `std::array<T, N>`),
    # not a runtime field — two arrays of the same element type but
    # different length are different, incompatible types.
    property array_size : Int32

    def initialize( @kind : TypeKind, @params = [] of Type, @ret : Type? = nil, @array_size = 0 )
    end

    INT8    = new TypeKind::Int8
    INT16   = new TypeKind::Int16
    INT32   = new TypeKind::Int32
    INT64   = new TypeKind::Int64
    INT     = new TypeKind::Int
    FLOAT   = new TypeKind::Float
    FLOAT32 = new TypeKind::Float32
    FLOAT64 = new TypeKind::Float64
    BOOL    = new TypeKind::Bool
    STR     = new TypeKind::Str
    REGEX   = new TypeKind::Regex
    NIL     = new TypeKind::Nil
    UNKNOWN = new TypeKind::Unknown
    UINT8   = new TypeKind::UInt8
    UINT16  = new TypeKind::UInt16
    UINT32  = new TypeKind::UInt32
    UINT64  = new TypeKind::UInt64
    UINT    = new TypeKind::UInt
    SYMBOL  = new TypeKind::Symbol

    def self.func( params : Array( Type ), ret : Type ) : Type
      new( TypeKind::Func, params, ret )
    end

    def self.pointer( pointee : Type ) : Type
      new( TypeKind::Pointer, [pointee] )
    end

    def self.array( element : Type, size : Int32 ) : Type
      new( TypeKind::Array, [element], array_size: size )
    end

    def pointee : Type
      params[0]
    end

    def element : Type
      params[0]
    end

    def pointer? : Bool
      kind.pointer?
    end

    def array? : Bool
      kind.array?
    end

    def symbol? : Bool
      kind.symbol?
    end

    def void_pointer? : Bool
      pointer? && pointee.nil_type?
    end

    def int64? : Bool
      kind.int64?
    end

     def uint8? : Bool
      kind.u_int8?
    end

    def uint16? : Bool
      kind.u_int16?
    end

    def uint32? : Bool
      kind.u_int32?
    end

    def uint64? : Bool
      kind.u_int64?
    end

    def uint? : Bool
      kind.u_int?
    end

    def signed? : Bool
      kind.int8? || kind.int16? || kind.int32? || kind.int64? || kind.int?
    end

    def unsigned? : Bool
      kind.u_int8? || kind.u_int16? || kind.u_int32? || kind.u_int64? || kind.u_int?
    end

    def numeric? : Bool
      integer? || float?
    end

    def integer? : Bool
      kind.int8? || kind.int16? || kind.int32? || kind.int64? || kind.int? ||
      kind.u_int8? || kind.u_int16? || kind.u_int32? || kind.u_int64? || kind.u_int?
    end

    def float? : Bool
      kind.float? || kind.float32? || kind.float64?
    end

    def nominal? : Bool
      kind.object? || kind.struct?
    end

    # `kind == TypeKind::Nil` spelled as a method. Never write `kind.nil?` for
    # this test : that resolves to `Object#nil?` (always false), not the enum
    # predicate — the classic dead-branch trap on enums with a `Nil` member.
    def nil_type? : Bool
      kind == TypeKind::Nil
    end

    def reference? : Bool
      kind.object? || kind.str? || kind.regex? || kind.func?
    end

    def byte_size : Int32
      case kind
      when .int8?, .u_int8?, .bool? then 1
      when .int16?, .u_int16?       then 2
      when .int32?, .u_int32?, .float32? then 4
      when .array?                      then element.byte_size * array_size
      else                              8   # Int64, UInt64, Int, UInt, Float, Float64, Str, Object, Struct-ref, Func, Regex, Nil, Pointer, Unknown
      end
    end

    def alignment : Int32
      byte_size
    end

    def int_bit_width : Int32
      case kind
      when .int8?, .u_int8?   then 8
      when .int16?, .u_int16? then 16
      when .int32?, .u_int32? then 32
      when .int64?, .int?, .u_int64?, .u_int? then 64
      else                        0
      end
    end

    def ==( other : Type ) : Bool
      if integer? && other.integer?
        return true
      end
      if float? && other.float?
        return true
      end
      return false unless kind == other.kind
      if kind.pointer?
        return pointee == other.pointee
      end
      if kind.array?
        return array_size == other.array_size && element == other.element
      end
      return true unless kind.func?
      return false unless ( r = ret ) && ( o = other.ret ) && r == o
      params.size == other.params.size &&
        params.zip( other.params ).all? { |a, b| a == b }
    end

    def to_s( io : IO ) : Nil
      case kind
      when .int8?    then io << "Int8"
      when .u_int8?  then io << "UInt8"
      when .int16?   then io << "Int16"
      when .u_int16? then io << "UInt16"
      when .int32?   then io << "Int32"
      when .u_int32? then io << "UInt32"
      when .int64?   then io << "Int64"
      when .u_int64? then io << "UInt64"
      when .int?     then io << "Int"
      when .u_int?   then io << "UInt"
      when .float?   then io << "Float"
      when .float32? then io << "Float32"
      when .float64? then io << "Float64"
      when .bool?    then io << "Bool"
      when .str?     then io << "String"
      when .regex?   then io << "Regex"
      when .symbol?  then io << "Symbol"
      # Enum constant, not `.nil?` — that predicate resolves to `Object#nil?`
      # (always false), which made this arm dead and `Nil` print as nothing.
      when TypeKind::Nil then io << "Nil"
      when .object?  then io << "Object"
      when .struct?  then io << "Struct"
      when .pointer?
        pointee.to_s( io )
        io << "*"
      when .array?
        element.to_s( io )
        io << "[" << array_size << "]"
      when .unknown? then io << "?"
      when .func?
        io << "(" << params.map( &.to_s ).join( ", " ) << ") -> " << ret
      end
    end

    # The type-table names a method call on a primitive receiver resolves
    # through, in lookup order: the exact width first (`"Int32"`), then the
    # inferred family (`"Int"`). These are the names a Core (or user)
    # `struct Int … end` reopening may be declared under. Non-primitive and
    # pointer types have no reopening — they resolve nominally.
    def reopen_names : Array( String )
      case kind
      when .int?, .int8?, .int16?, .int32?, .int64?
        kind.int? ? [ "Int" ] : [ to_s, "Int" ]
      when .u_int?, .u_int8?, .u_int16?, .u_int32?, .u_int64?
        kind.u_int? ? [ "UInt", "Int" ] : [ to_s, "UInt", "Int" ]
      when .float?, .float32?, .float64?
        kind.float? ? [ "Float" ] : [ to_s, "Float" ]
      when .bool?
        [ "Bool" ]
      when .regex?
        [ "Regex" ]
      when .symbol?
        [ "Symbol" ]
      when .pointer?
        [ "Pointer[#{pointee.to_s}]", "Pointer" ]
      when .array?
        # `[]`, `[]=`, `.size` are compiler intrinsics (`TypeChecker#infer_index`
        # et al.) resolved directly from `array_size`/`element`, never through
        # this table — a single reopened `struct Array … end` is shared across
        # every element type and length, so it can only carry what's true of
        # *any* array regardless of `T`/`N` (in practice: `include Inspectable`).
        [ "Array" ]
      else
        [] of String
      end
    end

    def self.from_primitive_name( name : String ) : Type?
      case name
      when "Int8"                          then INT8
      when "UInt8"                         then UINT8
      when "Int16"                         then INT16
      when "UInt16"                        then UINT16
      when "Int32"                         then INT32
      when "UInt32"                        then UINT32
      when "Int64"                         then INT64
      when "UInt64"                        then UINT64
      when "Int"                           then INT
      when "UInt"                          then UINT
      when "Float"                         then FLOAT
      when "Float32"                       then FLOAT32
      when "Float64"                       then FLOAT64
      when "Bool"                          then BOOL
      when "Nil", "Void"                   then NIL
      when "Regex"                         then REGEX
      when "Symbol"                        then SYMBOL
      else                                 nil
      end
    end

    # Resolve a parsed type annotation to a `Type`. `nominals`, when given, is
    # consulted for any name that isn't a built-in primitive : this is how
    # `class`/`struct` names (`Point`, `Device`, ...) resolve to their
    # `NominalType` once the semantic type-collection pass (Phase B) has run.
    def self.from_annotation( node : ATypeNode, nominals : Hash( String, NominalType )? = nil ) : Type?
      # `T?` (NilableType) : supported for class references only, and *erased*
      # to the underlying type — Tier-0 has no union types, but every class
      # reference is implicitly nilable at runtime (a `Value` slot can always
      # hold Nil), so the annotation is accepted as documentation and the
      # checker separately allows `nil` wherever an Object reference is
      # expected. `Int64?` / struct `?` stay unsupported: erasing those would
      # promise a nil the unboxed representation can't deliver.
      if node.is_a?( NilableType )
        inner = from_annotation( node.inner, nominals )
        return inner if inner && inner.kind.object?
        return nil
      end
      if node.is_a?( PointerType )
        inner = from_annotation( node.inner, nominals )
        return inner ? pointer( inner ) : nil
      end
      if node.is_a?( ArrayType )
        elem = from_annotation( node.elem, nominals )
        return elem ? array( elem, node.size ) : nil
      end
      # `Pair[String, Int64]` : a generic reference resolves to the nominal the
      # monomorphizer registered under its mangled name. Lookup-only : the
      # instantiation itself is triggered upstream (TypeCollector/TypeChecker),
      # so an unresolved generic here means it was never (or can't be)
      # instantiated and correctly reads as an unknown type.
      if node.is_a?( GenericType )
        return nil unless nominals
        args = [] of Type
        node.params.each do |p|
          arg = from_annotation( p, nominals )
          return nil unless arg
          args << arg
        end
        if node.name == "Pointer" && args.size == 1
          return pointer(args.first)
        end
        return nominals[ Monomorphizer.mangle( node.name, args ) ]?
      end
      return nil unless node.is_a?( SimpleType )
      from_primitive_name( node.name ) || nominals.try( &.[]?( node.name ) )
    end
  end


end
