module Volt::VM


  # Object-model family: allocation, field access, and struct value-copy
  # (architecture #1.B : no allocation for structs: they live directly in a
  # contiguous run of Frame registers; only class instances go through
  # `INIT_OBJ`/heap allocation).
  class Vm

    private def exec_object( frame : Frame, chunk : IR::Chunk, ins : IR::Instruction )
      case ins.op
      when .init_obj?   then exec_init_obj( frame, ins )
      when .load_field?  then frame[ ins.a ] = frame[ ins.b ].as_object.fields[ ins.c ]
      when .store_field? then frame[ ins.a ].as_object.fields[ ins.b ] = frame[ ins.c ]
      when .copy_block?  then exec_copy_block( frame, ins )
      when .new_struct?  then exec_new_struct( frame, ins )
      when .check_index? then exec_check_index( frame, ins )
      when .load_indexed?  then frame[ ins.a ] = frame[ ins.b + frame[ ins.c ].as_i.to_i32 ]
      when .store_indexed? then frame[ ins.a + frame[ ins.b ].as_i.to_i32 ] = frame[ ins.c ]
      else               end
    end

    # `Bx` = the array's compile-time-known length. Guards every dynamic
    # index (`LOAD_INDEXED`/`STORE_INDEXED`) against reading or writing past
    # the array's own register slots into whatever local happens to follow it
    # in the frame — the one thing the VM itself must enforce, since a
    # runtime-computed index can't be range-checked at compile time.
    private def exec_check_index( frame : Frame, ins : IR::Instruction )
      idx = frame[ ins.a ].as_i
      if idx < 0 || idx >= ins.bx
        raise VoltRuntimeError.new( "index out of bounds: index #{idx}, size #{ins.bx}" )
      end
    end

    private def exec_init_obj( frame : Frame, ins : IR::Instruction )
      slots = @registry[ ins.bx ]?.try( &.slot_count ) || 0
      obj = IR::HeapObject.allocate( ins.bx, slots )
      if box = @class_boxes[ ins.bx ]?
        obj.class_ref = box.as( Void* )
      end
      frame[ ins.a ] = IR::Value.object( obj )
    end

    # `C` = slot count. Copies register-to-register (a local struct
    # reassignment or a struct passed/returned by value) : a fresh,
    # independent block of values, not a shared reference.
    private def exec_copy_block( frame : Frame, ins : IR::Instruction )
      ins.c.times { |i| frame[ ins.a + i ] = frame[ ins.b + i ] }
    end

    # `Bx` = slot count. Reserves a struct's register block ahead of the
    # individual field stores that build it.
    private def exec_new_struct( frame : Frame, ins : IR::Instruction )
      ins.bx.times { |i| frame[ ins.a + i ] = IR::Value.nil_value }
    end

  end


end
