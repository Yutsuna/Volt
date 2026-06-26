module Volt::VM


  # Call-family helpers. The calling convention places arguments in registers
  # A+1 .. A+C and writes the result back into register A.
  class Vm
    def collect_args( frame : Frame, ins : IR::Instruction ) : Array( IR::Value )
      base = ins.a
      argc = ins.c
      args = Array( IR::Value ).new( argc )
      argc.times { |i| args << frame[ base + 1 + i ] }
      args
    end
  end


end
