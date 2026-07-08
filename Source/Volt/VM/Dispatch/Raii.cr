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
      slots = @registry[ ins.bx ]?.try( &.slot_count ) || 0
      obj = IR::HeapObject.allocate( ins.bx, slots )
      if box = @class_boxes[ ins.bx ]?
        obj.class_ref = box.as( Void* )
      end
      frame[ ins.a ] = IR::Value.object( obj )
    end

    private def exec_raii_drop( frame : Frame, chunk : IR::Chunk, ins : IR::Instruction )
      val = frame[ ins.a ]
      unless val.is_nil?
        destroy_object( val.as_object )
        frame[ ins.a ] = IR::Value.nil_value
      end
    end

    private def exec_raii_drop_scope( frame : Frame, chunk : IR::Chunk, ins : IR::Instruction )
      regs = chunk.scope_tables[ ins.bx ]
      regs.each do |r|
        val = frame[ r ]
        unless val.is_nil?
          destroy_object( val.as_object )
          frame[ r ] = IR::Value.nil_value
        end
      end
    end

    # Deterministic destruction (architecture #4.2): the user `finalize`
    # (only meaningful once the JIT/exception-unwind path distinguishes
    # "fully constructed" : Phase 5) followed by the compiler-generated
    # `__drop_fields`, which recursively drops any reference fields.
    private def destroy_object( obj : IR::HeapObject )
      return if obj.destroyed?
      if rclass = @registry[ obj.type_id ]?
        self_arg = [ IR::Value.object( obj ) ]
        call_chunk( @unit.chunks[ rclass.finalize_index ], self_arg ) if rclass.finalize_index >= 0
        call_chunk( @unit.chunks[ rclass.drop_fields_index ], self_arg ) if rclass.drop_fields_index >= 0
      end
      obj.destroy
    end

  end


end
