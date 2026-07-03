require "./Frame"
require "./Dispatch/__all__"


module Volt::VM


  class VoltRuntimeError < Exception
  end


  # Tier-0 register virtual machine. Uses a `case` dispatch loop (architecture #7.1
  # implementation note: start with `case`, migrate to direct-threaded once the opcode
  # set is stable). Opcode families live in VM/Dispatch/*.
  class Vm

    def initialize( @unit : Compiler::Unit, @stdout : IO = STDOUT, @stderr : IO = STDERR )
      @registry = Runtime::ObjectModel::TypeRegistry.new( @unit.classes )
      # Process-global storage for module `@@vars` (`main` writes their
      # declared defaults before any other code runs).
      @globals  = Array( IR::Value ).new( @unit.num_globals ) { IR::Value.nil_value }
    end

    #--------------------------------------------------------------------------

    def run : Int32
      call_chunk( @unit.chunks[ @unit.main_index ], [] of IR::Value )
      0
    rescue e : VoltRuntimeError
      @stderr.puts( "runtime error: #{e.message || "unknown"}" )
      1
    rescue e : DivisionByZeroError
      @stderr.puts( "runtime error: division by zero" )
      1
    end

    #--------------------------------------------------------------------------

    def call_chunk( chunk : IR::Chunk, args : Array( IR::Value ) ) : Array( IR::Value )
      frame = Frame.new( chunk, args )
      code  = chunk.code
      ip    = 0

      begin
        while ip < code.size
          ins = code[ ip ]
          ip += 1

          case ins.op
          in .load_const?, .load_true?, .load_false?, .load_nil?, .move?
            exec_load_store(frame, chunk, ins)

          in .jmp?
            ip = ins.bx

          in .jmp_if_false?
            ip = ins.bx unless frame[ins.a].truthy?

          in .not?
            frame[ins.a] = IR::Value.bool(!frame[ins.b].truthy?)

          in .conv_int?
            exec_conv_int(frame, ins)

          in .raise?
            raise VoltRuntimeError.new(frame[ins.a].to_display)

          in .call?
            results = call_chunk(@unit.chunks[ins.b], collect_args(frame, ins))
            results.each_with_index { |v, i| frame[ins.a + i] = v }

          in .call_native?
            frame[ins.a] = call_native(@unit.natives[ins.b], collect_args(frame, ins))

          in .ret?
            slots = ins.bx > 0 ? ins.bx : 1
            return Array( IR::Value ).new( slots ) { |i| frame[ins.a + i] }

          in .eq?, .ne?, .lt_int?, .le_int?, .gt_int?, .ge_int?,
              .lt_f64?, .le_f64?, .gt_f64?, .ge_f64?, .cmp_int?, .eq_case?,
              .match_str?, .not_match_str?
            frame[ins.a] = eval_cmp(ins.op, frame[ins.b], frame[ins.c])

          in .init?, .drop?, .drop_scope?
            exec_raii(frame, chunk, ins)

          in .add_int?, .sub_int?, .mul_int?, .div_int?, .mod_int?, .neg_int?,
              .add_f64?, .sub_f64?, .mul_f64?, .div_f64?, .neg_f64?,
              .and_int?, .or_int?, .xor_int?, .shl_int?, .shr_int?, .not_int?,
              .idiv_int?, .pow_int?
            frame[ins.a] = eval_arith(ins.op, frame[ins.b], frame[ins.c])

          in .init_obj?, .load_field?, .store_field?, .copy_block?, .new_struct?
            exec_object( frame, chunk, ins )

          in .call_method?
            # Same arg window as `.call?` (`collect_args` reads C slots from
            # A+1..) : the receiver travels as that first, always-1-slot,
            # argument. B is the vtable slot index, resolved statically at
            # compile time from the receiver's *static* type — dispatch
            # reads the *actual* object's own class's vtable at that same
            # slot, which is what makes it virtual (architecture #7.3).
            obj    = frame[ins.a + 1].as_object
            rclass = @registry[obj.type_id]?
            raise VoltRuntimeError.new( "unknown class for type_id #{obj.type_id}" ) unless rclass
            chunk_idx = rclass.vtable[ins.b]?
            if chunk_idx.nil? || chunk_idx < 0
              raise VoltRuntimeError.new( "unresolved virtual method (vtable slot #{ins.b}) on #{rclass.name}" )
            end
            results = call_chunk(@unit.chunks[chunk_idx], collect_args(frame, ins))
            results.each_with_index { |v, i| frame[ins.a + i] = v }

          in .call_mixin?
            # ITable dispatch — reserved, unused (mixin methods are compiled
            # once per including class and dispatched through the same
            # vtable as any other instance method, see `BytecodeCompiler`).
            raise VoltRuntimeError.new( "opcode #{ins.op} is not yet implemented" )

          in .load_global?
            frame[ins.a] = @globals[ins.bx]

          in .store_global?
            @globals[ins.bx] = frame[ins.a]

          in .to_string?
            frame[ins.a] = IR::Value.str(frame[ins.b].to_display)

          in .concat_str?
            frame[ins.a] = IR::Value.str(frame[ins.b].as_s + frame[ins.c].as_s)
          end
        end
      ensure
        unwind_frame(frame, chunk, ip) if ip < code.size
      end

      [ IR::Value.nil_value ]
    end

    #--------------------------------------------------------------------------

    private def exec_conv_int( frame : Frame, ins : IR::Instruction )
      val = frame[ ins.a ].as_i
      shift = 64 - ins.bx
      extended = ( val << shift ) >> shift
      frame[ ins.a ] = IR::Value.int( extended )
    end

    # Walks the DropMap and destroys any live RAII objects whose pc range covers
    # the current instruction pointer.
    # architecture #4.6
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
