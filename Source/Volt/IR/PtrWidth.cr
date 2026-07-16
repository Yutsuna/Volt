module Volt::IR


  enum PtrWidth : UInt8
    I8  = 0
    I16 = 1
    I32 = 2
    I64 = 3
    U8  = 4
    U16 = 5
    U32 = 6
    U64 = 7
    PTR = 8
    F32 = 9
    F64 = 10
    OBJ = 11   # class-instance reference : payload pointer + VAL_OBJECT tag

    def self.for( type : Frontend::Type ) : PtrWidth
      case type.kind
      when .int8?           then I8
      when .int16?          then I16
      when .int32?          then I32
      when .int64?, .int?   then I64
      # A symbol is its interned Int64 id at runtime (`Frontend::Symbols`).
      when .symbol?          then I64
      when .u_int8?, .bool?  then U8
      when .u_int16?         then U16
      when .u_int32?         then U32
      when .u_int64?, .u_int? then U64
      when .pointer?         then PTR
      when .float32?         then F32
      when .float64?, .float? then F64
      # A class reference round-trips through memory as its raw heap pointer;
      # the OBJ width restores the VAL_OBJECT tag on load (a bare PTR-tagged
      # reference would fail every `Tag == VAL_OBJECT` receiver check).
      # Legacy `Type::STR` values are Core `String` instances at runtime too.
      when .object?, .str?   then OBJ
      else
        raise "internal: unsupported type for pointer load/store: #{type}"
      end
    end
  end


end
