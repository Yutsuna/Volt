require "../../IR/Value"

# FFI binding to the C direct-threaded dispatch core (Source/C).
@[Link(ldflags: "#{__DIR__}/../../../C/libvoltvm.so")]
lib LibVoltDispatch
  # Mirror of C `FChunkInfo` (Dispatch.h) — one per Unit chunk.
  struct ChunkInfo
    code           : Void*   # UInt32*  : chunk.code buffer
    code_size      : Int32
    consts         : Void*   # IR::Value* : chunk.constants buffer
    num_registers  : Int32
    raii_regs      : Void*   # UInt8*   : registers to nil on frame entry
    raii_regs_count : Int32
    has_drop_map   : UInt8
  end

  # Mirror of C `FCallFrame` — one suspended caller on the shared frame stack.
  struct CallFrame
    chunk_index : Int32
    base        : Int32
    ip          : Int32
  end

  # Mirror of C `FVmContext` — mutable cursor + environment. Field order and
  # types MUST match Dispatch.h exactly (C ABI layout).
  struct VmContext
    chunks         : Void*   # ChunkInfo*
    chunk_count    : Int32
    stack          : Void*   # IR::Value*
    stack_capacity : Int32
    globals        : Void*   # IR::Value*
    global_count   : Int32
    frames         : Void*   # CallFrame*
    frame_cap      : Int32
    cur_chunk      : Int32
    base           : Int32
    ip             : Int32
    frame_depth    : Int32
    stack_top      : Int32
    cold_op        : Int32
    result_slots   : Int32
  end

  # Runs the threaded core; returns an EVmStatus (see Volt::VM::DispatchStatus).
  fun dispatch = Volt_Dispatch( ctx : VmContext* ) : Int32
end


module Volt::VM


  # Return codes of `LibVoltDispatch.dispatch` — mirror of C `EVmStatus`.
  module DispatchStatus
    HALT           =  0
    CALLBACK       =  1
    ERR_DIVZERO    =  2
    ERR_STACKOVER  =  3
    ERR_BADOP      =  4
    ERR_OVERFLOW   =  5
  end


end
