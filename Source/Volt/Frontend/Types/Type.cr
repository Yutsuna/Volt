module Volt::Frontend


  enum TypeKind
    Int8
    Int16
    Int32
    Int
    Float
    Bool
    Str
    Regex
    Nil
    Func
    Unknown
  end


  class Type
    property kind   : TypeKind
    property params : Array( Type )
    property ret    : Type?

    def initialize( @kind : TypeKind, @params = [] of Type, @ret : Type? = nil )
    end

    INT8    = new( TypeKind::Int8 )
    INT16   = new( TypeKind::Int16 )
    INT32   = new( TypeKind::Int32 )
    INT     = new( TypeKind::Int )
    FLOAT   = new( TypeKind::Float )
    BOOL    = new( TypeKind::Bool )
    STR     = new( TypeKind::Str )
    REGEX   = new( TypeKind::Regex )
    NIL     = new( TypeKind::Nil )
    UNKNOWN = new( TypeKind::Unknown )

    def self.func( params : Array( Type ), ret : Type ) : Type
      new( TypeKind::Func, params, ret )
    end

    def numeric? : Bool
      kind.int8? || kind.int16? || kind.int32? || kind.int? || kind.float?
    end

    def integer? : Bool
      kind.int8? || kind.int16? || kind.int32? || kind.int?
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
      when .bool?    then io << "Bool"
      when .str?     then io << "String"
      when .regex?   then io << "Regex"
      when .nil?     then io << "Nil"
      when .unknown? then io << "?"
      when .func?
        io << "(" << params.map( &.to_s ).join( ", " ) << ") -> " << ret
      end
    end

    def self.from_annotation( node : ATypeNode ) : Type?
      return nil unless node.is_a?( SimpleType )
      case node.name
      when "Int8"                          then INT8
      when "Int16"                         then INT16
      when "Int32"                         then INT32
      when "Int", "Int64"                  then INT
      when "Float", "Float32", "Float64"   then FLOAT
      when "Bool"                          then BOOL
      when "String"                        then STR
      when "Nil", "Void"                   then NIL
      when "Regex"                         then REGEX
      else                                      nil
      end
    end
  end


end
