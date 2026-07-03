module Volt::IR


  # One resolved `CALL_MIXIN` call site. A mixin dispatch needs four pieces of
  # information (module id to look up in the receiver's ITable, the method's
  # offset within that module's vtable slice, and the register window), which
  # don't fit in a single ABC/ABx instruction : so `CALL_MIXIN` carries a `Bx`
  # index into `Chunk#call_sites` instead of encoding operands directly.
  struct CallSite
    property module_id     : Int32
    property method_offset : Int32
    property base_reg      : UInt8
    property dest_reg      : UInt8

    def initialize( @module_id : Int32, @method_offset : Int32, @base_reg : UInt8, @dest_reg : UInt8 )
    end
  end


end
