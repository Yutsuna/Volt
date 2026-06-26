module Volt::Frontend


  enum TypeKind
    Int
    Float
    Bool
    Str
    Nil
    Func
    Unknown
  end


  # Resolved type produced by the Semantic pass and attached to every AExpr.
  # Primitives are shared singletons; `Func` carries its signature.
  class Type
    property kind   : TypeKind
    property params : Array( Type )
    property ret    : Type?

    def initialize( @kind : TypeKind, @params = [] of Type, @ret : Type? = nil )
    end

    INT     = new( TypeKind::Int )
    FLOAT   = new( TypeKind::Float )
    BOOL    = new( TypeKind::Bool )
    STR     = new( TypeKind::Str )
    NIL     = new( TypeKind::Nil )
    UNKNOWN = new( TypeKind::Unknown )

    def self.func( params : Array( Type ), ret : Type ) : Type
      new( TypeKind::Func, params, ret )
    end

    def numeric? : Bool
      kind.int? || kind.float?
    end

    def ==( other : Type ) : Bool
      return false unless kind == other.kind
      return true unless kind.func?
      return false unless ( r = ret ) && ( o = other.ret ) && r == o
      params.size == other.params.size &&
        params.zip( other.params ).all? { |a, b| a == b }
    end

    def to_s( io : IO ) : Nil
      case kind
      when .int?     then io << "Int"
      when .float?   then io << "Float"
      when .bool?    then io << "Bool"
      when .str?     then io << "String"
      when .nil?     then io << "Nil"
      when .unknown? then io << "?"
      when .func?
        io << "(" << params.map( &.to_s ).join( ", " ) << ") -> " << ret
      end
    end

    # Maps a written annotation (SimpleType) to a resolved primitive Type.
    def self.from_annotation( node : ATypeNode ) : Type?
      return nil unless node.is_a?( SimpleType )
      case node.name
      when "Int", "Int32", "Int64", "Int8" then INT
      when "Float", "Float32", "Float64"   then FLOAT
      when "Bool"                          then BOOL
      when "String"                        then STR
      when "Nil"                           then NIL
      else                                      nil
      end
    end
  end


end
