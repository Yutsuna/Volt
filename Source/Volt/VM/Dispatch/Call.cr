module Volt::VM


  # Call-family helpers. The calling convention places arguments in registers
  # A+1 .. A+C and writes the result back into register A.
  class Vm
    # Zero-copy view over the callee's argument registers (Phase 5: was an
    # `Array` copy, now a `Slice` window straight into `Vm@stack` — only
    # `CALL_NATIVE` still calls this, `CALL`/`CALL_METHOD` use the windowed
    # calling convention directly since Phase 1).
    def collect_args( frame : Frame, ins : IR::Instruction ) : Slice( IR::Value )
      frame.slice( ins.a + 1, ins.c )
    end
  end


end
