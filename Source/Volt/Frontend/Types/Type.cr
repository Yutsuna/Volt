module Volt::Frontend


  class Type
    property kind   : TypeKind
    property params : Array( Type )
    property ret    : Type?

    def initialize( @kind : TypeKind, @params = [] of Type, @ret : Type? = nil )
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

    def self.func( params : Array( Type ), ret : Type ) : Type
      new( TypeKind::Func, params, ret )
    end

    def self.pointer( pointee : Type ) : Type
      new( TypeKind::Pointer, [pointee] )
    end

    def pointee : Type
      params[0]
    end

    def pointer? : Bool
      kind.pointer?
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
      # Enum constant, not `.nil?` — that predicate resolves to `Object#nil?`
      # (always false), which made this arm dead and `Nil` print as nothing.
      when TypeKind::Nil then io << "Nil"
      when .object?  then io << "Object"
      when .struct?  then io << "Struct"
      when .pointer?
        pointee.to_s( io )
        io << "*"
      when .unknown? then io << "?"
      when .func?
        io << "(" << params.map( &.to_s ).join( ", " ) << ") -> " << ret
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
      return nil unless node.is_a?( SimpleType )
      case node.name
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
      when "String"                        then STR
      when "Nil", "Void"                   then NIL
      when "Regex"                         then REGEX
      else                                      nominals.try( &.[]?( node.name ) )
      end
    end
  end


end
