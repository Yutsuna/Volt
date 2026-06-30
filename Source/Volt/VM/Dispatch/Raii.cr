module Volt::VM


  # RAII family (INIT / DROP / DROP_SCOPE architecture #4.2).
  class Vm

    private def exec_raii( frame : Frame, chunk : IR::Chunk, ins : IR::Instruction )
      case ins.op
      when .init?       then exec_raii_init(frame, chunk, ins)
      when .drop?       then exec_raii_drop(frame, chunk, ins)
      when .drop_scope? then exec_raii_drop_scope(frame, chunk, ins)
      else              end
    end

    private def exec_raii_init( frame : Frame, chunk : IR::Chunk, ins : IR::Instruction )
      frame[ ins.a ] = IR::Value.object( IR::HeapObject.new( ins.bx ) )
    end

    private def exec_raii_drop( frame : Frame, chunk : IR::Chunk, ins : IR::Instruction )
      val = frame[ ins.a ]
      unless val.is_nil?
        val.as_object.destroy
        frame[ ins.a ] = IR::Value.nil_value
      end
    end

    private def exec_raii_drop_scope( frame : Frame, chunk : IR::Chunk, ins : IR::Instruction )
      regs = chunk.scope_tables[ ins.bx ]
      regs.each do |r|
        val = frame[ r ]
        unless val.is_nil?
          val.as_object.destroy
          frame[ r ] = IR::Value.nil_value
        end
      end
    end

  end


end
