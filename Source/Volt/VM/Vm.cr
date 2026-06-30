module Volt::VM


  class VoltRuntimeError < Exception
  end


  # Tier-0 register virtual machine. Uses a `case` dispatch loop (architecture #7.1
  # implementation note: start with `case`, migrate to direct-threaded once the opcode
  # set is stable). Opcode families live in VM/Dispatch/*.
  class Vm
    def initialize( @unit : Compiler::Unit )
    end

    # Runs the entry chunk; returns a process exit code.
    def run : Int32
      call_chunk( @unit.chunks[ @unit.main_index ], [] of IR::Value )
      0
    rescue e : VoltRuntimeError
      STDERR.puts( "runtime error: #{e.message || "unknown"}" )
      1
    rescue e : DivisionByZeroError
      STDERR.puts( "runtime error: division by zero" )
      1
    end

    # Executes a chunk with the given arguments and returns its result value.
    def call_chunk( chunk : IR::Chunk, args : Array( IR::Value ) ) : IR::Value
      frame = Frame.new( chunk, args )
      code  = chunk.code
      ip    = 0

      begin
        while ip < code.size
          ins = code[ ip ]
          ip += 1

          case ins.op
          when .load_const?, .load_true?, .load_false?, .load_nil?, .move?
            exec_load_store( frame, chunk, ins )
          when .jmp?
            ip = ins.bx
          when .jmp_if_false?
            ip = ins.bx unless frame[ ins.a ].truthy?
          when .not?
            frame[ ins.a ] = IR::Value.bool( !frame[ ins.b ].truthy? )
          when .conv_int?
            val = frame[ ins.a ].as_i
            bit_width = ins.bx
            shift = 64 - bit_width
            extended = ( val << shift ) >> shift
            frame[ ins.a ] = IR::Value.int( extended )
          when .raise?
            msg = frame[ ins.a ].to_display
            raise VoltRuntimeError.new( msg )
          when .call?
            frame[ ins.a ] = call_chunk( @unit.chunks[ ins.b ], collect_args( frame, ins ) )
          when .call_native?
            frame[ ins.a ] = call_native( @unit.natives[ ins.b ], collect_args( frame, ins ) )
          when .ret?
            return frame[ ins.a ]
          when .eq?, .ne?, .lt_int?, .le_int?, .gt_int?, .ge_int?,
               .lt_f64?, .le_f64?, .gt_f64?, .ge_f64?, .cmp_int?, .eq_case?,
               .match_str?, .not_match_str?
            frame[ ins.a ] = eval_cmp( ins.op, frame[ ins.b ], frame[ ins.c ] )
          when .init?, .drop?, .drop_scope?
            exec_raii( frame, chunk, ins )
          else
            # arithmetic family (ADD/SUB/MUL/DIV/MOD/NEG, int + f64)
            frame[ ins.a ] = eval_arith( ins.op, frame[ ins.b ], frame[ ins.c ] )
          end
        end
      rescue e : VoltRuntimeError
        unwind_frame( frame, chunk, ip )
        raise e
      end

      IR::Value.nil_value
    end

    # Walks the DropMap and destroys any live RAII objects whose pc range covers
    # the current instruction pointer. Used for deterministic cleanup on exception
    # unwind (architecture #4.6).
    private def unwind_frame( frame : Frame, chunk : IR::Chunk, ip : Int32 )
      chunk.drop_map.entries.each do |entry|
        pc = ip.to_u32
        if pc >= entry.pc_start && pc <= entry.pc_end
          val = frame[ entry.register.to_i32 ]
          unless val.is_nil?
            val.as_object.destroy
            frame[ entry.register.to_i32 ] = IR::Value.nil_value
          end
        end
      end
    end
  end


end
