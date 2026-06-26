module Volt::VM


  # Branch family (JMP / JMP_IF_FALSE). These mutate the interpreter's instruction
  # pointer, so they are handled inline in Vm#call_chunk rather than via a helper.
  # This file documents the family and is the home for future branch opcodes.
  class Vm
  end


end
