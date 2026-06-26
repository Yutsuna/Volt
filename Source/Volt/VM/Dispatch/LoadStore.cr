module Volt::VM


  # Load / move opcode handlers.
  class Vm
    def exec_load_store( frame : Frame, chunk : IR::Chunk, ins : IR::Instruction )
      case ins.op
      when .load_const? then frame[ ins.a ] = chunk.constants[ ins.bx ]
      when .load_true?  then frame[ ins.a ] = IR::Value.bool( true )
      when .load_false? then frame[ ins.a ] = IR::Value.bool( false )
      when .load_nil?   then frame[ ins.a ] = IR::Value.nil_value
      when .move?       then frame[ ins.a ] = frame[ ins.b ]
      else
        raise VoltRuntimeError.new( "unhandled load/store opcode #{ins.op}" )
      end
    end
  end


end
