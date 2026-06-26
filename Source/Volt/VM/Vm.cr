module Volt::VM


  class VoltRuntimeError < Exception
  end


  # Tier-0 register virtual machine. Uses a `case` dispatch loop (architecture §7.1
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
        when .call?
          frame[ ins.a ] = call_chunk( @unit.chunks[ ins.b ], collect_args( frame, ins ) )
        when .call_native?
          frame[ ins.a ] = call_native( @unit.natives[ ins.b ], collect_args( frame, ins ) )
        when .ret?
          return frame[ ins.a ]
        when .eq?, .ne?, .lt_int?, .le_int?, .gt_int?, .ge_int?,
             .lt_f64?, .le_f64?, .gt_f64?, .ge_f64?
          frame[ ins.a ] = eval_cmp( ins.op, frame[ ins.b ], frame[ ins.c ] )
        else
          # arithmetic family (ADD/SUB/MUL/DIV/MOD/NEG, int + f64)
          frame[ ins.a ] = eval_arith( ins.op, frame[ ins.b ], frame[ ins.c ] )
        end
      end

      IR::Value.nil_value
    end
  end


end
