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
    INT     = new TypeKind::Int
    FLOAT   = new TypeKind::Float
    FLOAT32 = new TypeKind::Float32
    FLOAT64 = new TypeKind::Float64
    BOOL    = new TypeKind::Bool
    STR     = new TypeKind::Str
    REGEX   = new TypeKind::Regex
    NIL     = new TypeKind::Nil
    UNKNOWN = new TypeKind::Unknown

    def self.func( params : Array( Type ), ret : Type ) : Type
      new( TypeKind::Func, params, ret )
    end

    def numeric? : Bool
      integer? || float?
    end

    def integer? : Bool
      kind.int8? || kind.int16? || kind.int32? || kind.int?
    end

    def float? : Bool
      kind.float? || kind.float32? || kind.float64?
    end

    def nominal? : Bool
      kind.object? || kind.struct?
    end

    def reference? : Bool
      kind.object? || kind.str? || kind.regex? || kind.func?
    end

    def byte_size : Int32
      case kind
      when .int8?, .bool?     then 1
      when .int16?            then 2
      when .int32?, .float32? then 4
      else                         8   # Int, Float, Float64, Str, Object, Struct-ref, Func, Regex, Nil, Unknown
      end
    end

    def alignment : Int32
      byte_size
    end

    def int_bit_width : Int32
      case kind
      when .int8?  then 8
      when .int16? then 16
      when .int32? then 32
      when .int?   then 64
      else              0
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
      return true unless kind.func?
      return false unless ( r = ret ) && ( o = other.ret ) && r == o
      params.size == other.params.size &&
        params.zip( other.params ).all? { |a, b| a == b }
    end

    def to_s( io : IO ) : Nil
      case kind
      when .int8?    then io << "Int8"
      when .int16?   then io << "Int16"
      when .int32?   then io << "Int32"
      when .int?     then io << "Int"
      when .float?   then io << "Float"
      when .float32? then io << "Float32"
      when .float64? then io << "Float64"
      when .bool?    then io << "Bool"
      when .str?     then io << "String"
      when .regex?   then io << "Regex"
      when .nil?     then io << "Nil"
      when .object?  then io << "Object"
      when .struct?  then io << "Struct"
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
      return nil unless node.is_a?( SimpleType )
      case node.name
      when "Int8"                          then INT8
      when "Int16"                         then INT16
      when "Int32"                         then INT32
      when "Int", "Int64"                  then INT
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
