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

    def self.for( type : Frontend::Type ) : PtrWidth
      case type.kind
      when .int8?           then I8
      when .int16?          then I16
      when .int32?          then I32
      when .int64?, .int?   then I64
      when .u_int8?, .bool?  then U8
      when .u_int16?         then U16
      when .u_int32?         then U32
      when .u_int64?, .u_int? then U64
      when .pointer?         then PTR
      else
        raise "internal: unsupported type for pointer load/store: #{type}"
      end
    end
  end


end
