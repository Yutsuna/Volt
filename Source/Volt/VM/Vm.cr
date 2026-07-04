require "./Frame"
require "./Dispatch/__all__"


module Volt::VM


  class VoltRuntimeError < Exception
  end


  # Tier-0 register virtual machine. Uses a `case` dispatch loop (architecture #7.1
  # implementation note: start with `case`, migrate to direct-threaded once the opcode
  # set is stable). Opcode families live in VM/Dispatch/*.
  class Vm

    # Shared contiguous register stack (architecture #7: the per-frame `Array`
    # is replaced by a single flat window stack). 1M `Value` slots = 16 MB.
    # Allocated through `Pointer.malloc` (i.e. `GC.malloc`) so the collector
    # scans it and any `String`/`HeapObject` reference parked in a register
    # stays alive.
    STACK_CAPACITY = 1 << 20

    def initialize( @unit : Compiler::Unit, @stdout : IO = STDOUT, @stderr : IO = STDERR )
      @registry = Runtime::ObjectModel::TypeRegistry.new( @unit.classes )
      # Process-global storage for module `@@vars` (`main` writes their
      # declared defaults before any other code runs).
      @globals  = Array( IR::Value ).new( @unit.num_globals ) { IR::Value.nil_value }
      @stack    = Pointer( IR::Value ).malloc( STACK_CAPACITY ) { IR::Value.nil_value }
      # First free slot above every active frame. Slot 0 is reserved as the
      # return landing pad for the outermost frame, so frames start at base ≥ 1.
      @stack_top = 1
      # Resolved `dlsym` pointer per `@unit.natives` index, lazily filled by
      # `resolve_native` on a native's first `CALL_NATIVE` (architecture #9
      # Phase 5 — dlopen/dlsym used to be redone on every call).
      @native_ptrs = Array( Void* ).new( @unit.natives.size, Pointer(Void).null )
    end

    #--------------------------------------------------------------------------

    def extend( unit : Compiler::Unit ) : Nil
      @unit.chunks.concat( unit.chunks )
      @unit.classes.concat( unit.classes )
      unit.classes.each { |c| @registry.register( c ) }

      if unit.natives.size > @unit.natives.size
        new_natives = unit.natives[ @unit.natives.size .. -1 ]
        @unit.natives.concat( new_natives )
        @native_ptrs.concat( Array( Void* ).new( new_natives.size, Pointer(Void).null ) )
      end

      @unit.num_globals = unit.num_globals
      if @unit.num_globals > @globals.size
        additional = @unit.num_globals - @globals.size
        @globals.concat( Array( IR::Value ).new( additional, IR::Value.nil_value ) )
      end
    end

    def call_chunk_at( index : Int32, args : Array( IR::Value ) = [] of IR::Value ) : Array( IR::Value )
      call_chunk( @unit.chunks[ index ], args )
    end

    #--------------------------------------------------------------------------

    def run : Int32
      execute( @unit.chunks[ @unit.main_index ], 1 )
      0
    rescue e : VoltRuntimeError
      @stderr.puts( "runtime error: #{e.message || "unknown"}" )
      1
    rescue e : DivisionByZeroError
      @stderr.puts( "runtime error: division by zero" )
      1
    end

    #--------------------------------------------------------------------------

    # Public entry point (specs, REPL, `destroy_object` reentrancy). Runs
    # `chunk` in a fresh window at the top of the shared stack and marshals the
    # result back into an `Array` : the boxing lives only on this cold boundary,
    # never on the hot `CALL` path (which is windowed and copy-free).
    def call_chunk( chunk : IR::Chunk, args : Array( IR::Value ) ) : Array( IR::Value )
      base = @stack_top
      base = 1 if base < 1
      args.each_with_index { |a, i| @stack[ base + i ] = a }
      n = execute( chunk, base )
      Array( IR::Value ).new( n ) { |i| @stack[ base - 1 + i ] }
    end

    #--------------------------------------------------------------------------

    # One suspended call frame on the interpreter's explicit frame stack
    # (architecture #7 Phase 6). A Volt `CALL` no longer recurses into a native
    # `execute` : it pushes the caller's resume state (this record) and rebinds
    # the loop's hot locals to the callee; a `RET` pops it back. `chunk` is kept
    # so `RET` can restore the caller's code/constants; `ip` is the caller's
    # *post-CALL* instruction pointer (the resume address).
    private record CallFrame,
      chunk : IR::Chunk,
      base  : Int32,
      ip    : Int32

    # Switch the loop's hot locals into a freshly-entered callee frame: push the
    # current frame's resume state, rebind chunk/base/code/size/consts/regs/frame,
    # advance `@stack_top`, reset `ip` to 0, and nil the callee's RAII registers
    # (recycled stack slots can hold stale objects). A *macro*, not a method, so
    # it can reassign the enclosing loop's locals in place. Used by CALL /
    # CALL_METHOD. #Phase 6
    private macro enter_callee( callee_chunk, callee_base )
      frames.push( CallFrame.new( chunk, base, ip ) )
      chunk  = {{ callee_chunk }}
      base   = {{ callee_base }}
      code   = chunk.code.to_unsafe
      size   = chunk.code.size
      consts = chunk.constants.to_unsafe
      regs   = @stack + base
      frame  = Frame.new( regs )
      @stack_top = base + chunk.num_registers
      ip     = 0
      _rr = chunk.raii_regs
      _ri = 0
      while _ri < _rr.size
        regs[ _rr[ _ri ] ] = IR::Value.nil_value
        _ri += 1
      end
    end

    # Restore the loop's hot locals to a caller frame popped off `frames` after
    # the callee returned. No RAII nil-init: the caller's registers were already
    # initialised when *it* was entered. #Phase 6
    private macro restore_caller( popped )
      chunk  = {{ popped }}.chunk
      base   = {{ popped }}.base
      ip     = {{ popped }}.ip
      code   = chunk.code.to_unsafe
      size   = chunk.code.size
      consts = chunk.constants.to_unsafe
      regs   = @stack + base
      frame  = Frame.new( regs )
      @stack_top = base + chunk.num_registers
    end

    # Interpret `chunk` in the register window starting at `base`. Returns the
    # number of result slots; the results are written to `@stack[base - 1 ..]`
    # (i.e. into the caller's result register), so a `CALL` is copy-free : the
    # caller placed the arguments at `base .. base + argc` (its own A+1..) and
    # reads the results back from register A.
    #
    # Phase 6: Volt calls run as iterations of *this* loop over an explicit
    # `frames` stack rather than native Crystal recursion — one native call +
    # `ensure` landing pad per Volt call was the dominant cost of call-heavy
    # code (fib). `execute` is still re-entrant (specs / REPL / `destroy_object`
    # via `call_chunk`): each invocation owns its own `frames`, so a nested call
    # never disturbs an outer loop's stack.
    private def execute( chunk : IR::Chunk, base : Int32 ) : Int32
      saved_top  = @stack_top
      @stack_top = base + chunk.num_registers

      # Explicit frame stack for this invocation (pre-sized past typical
      # recursion depth to avoid reallocation on the hot call path).
      frames = Array( CallFrame ).new( 256 )

      # Hot locals: a raw register-window pointer (index = no bounds check) and
      # unsafe views over the code/constant arrays, which are immutable for the
      # lifetime of this frame. `frame` is the same window, wrapped, for the
      # cold handlers that still take a `Frame`. All are rebound by
      # `enter_callee`/`restore_caller` on every Volt CALL/RET.
      regs   = @stack + base
      frame  = Frame.new( regs )
      code   = chunk.code.to_unsafe
      size   = chunk.code.size
      consts = chunk.constants.to_unsafe

      # Recycled stack slots hold stale Values from dead frames; nil the RAII
      # registers so unwind/early-return never destroys a stale object.
      rr = chunk.raii_regs
      i  = 0
      while i < rr.size
        regs[ rr[ i ] ] = IR::Value.nil_value
        i += 1
      end

      ip = 0

      begin
        while true
          while ip < size
            ins = code[ ip ]
            ip += 1
            op = ins.op

            # Single-level dispatch on the raw enum value: matching against enum
            # *constants* (not `.pred?` predicate methods) lets Crystal/LLVM lower
            # this to a dense `switch` / jump table instead of a linear if-chain.
            # Hot opcodes are inlined here (no family-handler call, no second
            # `case`); rare/cold ones delegate.
            case op
            # ---- load / move ----
            when IR::Opcode::LOAD_CONST  then regs[ins.a] = consts[ins.bx]
            when IR::Opcode::MOVE        then regs[ins.a] = regs[ins.b]
            when IR::Opcode::LOAD_TRUE   then regs[ins.a] = IR::Value.bool( true )
            when IR::Opcode::LOAD_FALSE  then regs[ins.a] = IR::Value.bool( false )
            when IR::Opcode::LOAD_NIL    then regs[ins.a] = IR::Value.nil_value

            # ---- arithmetic & comparison (delegated) ----
            # Kept out of line on purpose: inlining all ~30 arith/cmp bodies here
            # bloats the dispatch loop and pushes LLVM off the jump table.
            # `eval_arith`/`eval_cmp` are themselves jump-table sub-dispatches.
            when IR::Opcode::ADD_INT, IR::Opcode::SUB_INT, IR::Opcode::MUL_INT,
                 IR::Opcode::DIV_INT, IR::Opcode::MOD_INT, IR::Opcode::NEG_INT,
                 IR::Opcode::IDIV_INT, IR::Opcode::POW_INT, IR::Opcode::AND_INT,
                 IR::Opcode::OR_INT, IR::Opcode::XOR_INT, IR::Opcode::SHL_INT,
                 IR::Opcode::SHR_INT, IR::Opcode::NOT_INT, IR::Opcode::ADD_F64,
                 IR::Opcode::SUB_F64, IR::Opcode::MUL_F64, IR::Opcode::DIV_F64,
                 IR::Opcode::NEG_F64
              regs[ins.a] = eval_arith( op, regs[ins.b], regs[ins.c] )

            when IR::Opcode::LT_INT, IR::Opcode::LE_INT, IR::Opcode::GT_INT,
                 IR::Opcode::GE_INT, IR::Opcode::LT_F64, IR::Opcode::LE_F64,
                 IR::Opcode::GT_F64, IR::Opcode::GE_F64, IR::Opcode::EQ, IR::Opcode::NE,
                 IR::Opcode::EQ_INT, IR::Opcode::NE_INT
              regs[ins.a] = eval_cmp( op, regs[ins.b], regs[ins.c] )

            # ---- peephole superinstructions (Compiler::Peephole) ----
            when IR::Opcode::NOP # no-op padding

            when IR::Opcode::ADD_INT_IMM then regs[ins.a] = IR::Value.int( regs[ins.b].as_i + ins.c.to_i64 )
            when IR::Opcode::SUB_INT_IMM then regs[ins.a] = IR::Value.int( regs[ins.b].as_i - ins.c.to_i64 )
            when IR::Opcode::EQ_INT_IMM  then regs[ins.a] = IR::Value.bool( regs[ins.b].as_i == ins.c.to_i64 )
            when IR::Opcode::NE_INT_IMM  then regs[ins.a] = IR::Value.bool( regs[ins.b].as_i != ins.c.to_i64 )

            # Compare+branch fusion : never materialises a boolean `Value`. The
            # target is read from the *next* code slot (the original,
            # untouched `JMP_IF_FALSE` left behind by the peephole pass) rather
            # than encoded in this instruction (an ABC-form op has no room for
            # a 16-bit target alongside two register operands).
            when IR::Opcode::BR_LT_INT, IR::Opcode::BR_LE_INT, IR::Opcode::BR_GT_INT,
                 IR::Opcode::BR_GE_INT, IR::Opcode::BR_EQ_INT, IR::Opcode::BR_NE_INT
              if eval_br_cond( op, regs[ins.b], regs[ins.c] )
                ip += 1
              else
                ip = code[ip].bx
              end

            when IR::Opcode::BR_LT_INT_IMM, IR::Opcode::BR_LE_INT_IMM, IR::Opcode::BR_GT_INT_IMM,
                 IR::Opcode::BR_GE_INT_IMM, IR::Opcode::BR_EQ_INT_IMM, IR::Opcode::BR_NE_INT_IMM
              if eval_br_cond_imm( op, regs[ins.b], ins.c )
                ip += 1
              else
                ip = code[ip].bx
              end

            # ---- logical / control flow ----
            when IR::Opcode::NOT          then regs[ins.a] = IR::Value.bool( !regs[ins.b].truthy? )
            when IR::Opcode::JMP          then ip = ins.bx
            when IR::Opcode::JMP_IF_FALSE then ip = ins.bx unless regs[ins.a].truthy?

            # ---- module globals ----
            when IR::Opcode::LOAD_GLOBAL  then regs[ins.a] = @globals[ins.bx]
            when IR::Opcode::STORE_GLOBAL then @globals[ins.bx] = regs[ins.a]

            # ---- calls ----
            when IR::Opcode::CALL
              callee = @unit.chunks[ins.b]
              cbase  = base + ins.a + 1
              if cbase + callee.num_registers > STACK_CAPACITY
                raise VoltRuntimeError.new( "stack overflow" )
              end
              # Args are already contiguous at regs[A+1..] == @stack[cbase..];
              # results land at @stack[cbase-1..] == regs[A..]. Copy-free. Enter
              # the callee in-loop rather than recursing.
              enter_callee( callee, cbase )

            when IR::Opcode::RET
              slots = ins.bx > 0 ? ins.bx : 1
              j = 0
              while j < slots
                @stack[ base - 1 + j ] = regs[ins.a + j]
                j += 1
              end
              # Early-return RAII drops (only when this RET isn't the terminal
              # instruction — matches the old per-frame `ensure` guard). Runs
              # for the returning frame *before* it is popped.
              unwind_frame( frame, chunk, ip ) if ip < size && !chunk.drop_map.empty?
              return slots if frames.empty?
              resumed = frames.pop
              restore_caller( resumed )

            when IR::Opcode::CALL_METHOD
              # Windowed like `CALL` : receiver travels as the first argument
              # (regs[A+1]). B is the vtable slot index, resolved statically from
              # the receiver's *static* type — dispatch reads the *actual*
              # object's own class's vtable at that slot (architecture #7.3).
              obj    = regs[ins.a + 1].as_object
              rclass = @registry[obj.type_id]?
              raise VoltRuntimeError.new( "unknown class for type_id #{obj.type_id}" ) unless rclass
              chunk_idx = rclass.vtable[ins.b]?
              if chunk_idx.nil? || chunk_idx < 0
                raise VoltRuntimeError.new( "unresolved virtual method (vtable slot #{ins.b}) on #{rclass.name}" )
              end
              callee = @unit.chunks[chunk_idx]
              cbase  = base + ins.a + 1
              if cbase + callee.num_registers > STACK_CAPACITY
                raise VoltRuntimeError.new( "stack overflow" )
              end
              enter_callee( callee, cbase )

            # ---- string builtins ----
            when IR::Opcode::TO_STRING  then regs[ins.a] = IR::Value.str( regs[ins.b].to_display )
            when IR::Opcode::CONCAT_STR then regs[ins.a] = IR::Value.str( regs[ins.b].as_s + regs[ins.c].as_s )

            when IR::Opcode::RAISE
              raise VoltRuntimeError.new( regs[ins.a].to_display )

            # ---- cold / rare : delegate to family handlers ----
            when IR::Opcode::CONV_INT
              exec_conv_int( frame, ins )

            when IR::Opcode::CALL_NATIVE
              regs[ins.a] = call_native( ins.b, collect_args( frame, ins ) )

            when IR::Opcode::CMP_INT, IR::Opcode::EQ_CASE, IR::Opcode::MATCH_STR, IR::Opcode::NOT_MATCH_STR
              regs[ins.a] = eval_cmp( op, regs[ins.b], regs[ins.c] )

            when IR::Opcode::INIT, IR::Opcode::DROP, IR::Opcode::DROP_SCOPE
              exec_raii( frame, chunk, ins )

            when IR::Opcode::INIT_OBJ, IR::Opcode::LOAD_FIELD, IR::Opcode::STORE_FIELD,
                 IR::Opcode::COPY_BLOCK, IR::Opcode::NEW_STRUCT
              exec_object( frame, chunk, ins )

            else
              # CALL_MIXIN (reserved/unused) and any unhandled opcode.
              raise VoltRuntimeError.new( "opcode #{op} is not yet implemented" )
            end
          end

          # Fell off the end without an explicit RET : implicit nil result. (No
          # early-drop unwind — matches the old `ensure` guard, which only fired
          # for `ip < size`.) Pop back to the caller, or finish if outermost.
          @stack[ base - 1 ] = IR::Value.nil_value
          return 1 if frames.empty?
          resumed = frames.pop
          restore_caller( resumed )
        end
      rescue ex
        # Exception unwind: run RAII drops for the faulting frame and every
        # suspended caller, deepest first — mirrors the per-frame `ensure` chain
        # of the old recursive `execute`. `unwind_frame` is a shallow destroy and
        # never re-enters the VM, so walking the stack here is safe.
        unwind_frame( frame, chunk, ip ) if ip < size && !chunk.drop_map.empty?
        while susp = frames.pop?
          schunk = susp.chunk
          if susp.ip < schunk.code.size && !schunk.drop_map.empty?
            unwind_frame( Frame.new( @stack + susp.base ), schunk, susp.ip )
          end
        end
        raise ex
      ensure
        @stack_top = saved_top
      end
    end

    #--------------------------------------------------------------------------

    # Peephole-fused compare+branch condition, evaluated straight to a native
    # `Bool` -- unlike `eval_cmp`, no `IR::Value` is boxed for a result that
    # would only ever be tested and discarded.
    private def eval_br_cond( op : IR::Opcode, b : IR::Value, c : IR::Value ) : Bool
      case op
      when IR::Opcode::BR_LT_INT then b.as_i < c.as_i
      when IR::Opcode::BR_LE_INT then b.as_i <= c.as_i
      when IR::Opcode::BR_GT_INT then b.as_i > c.as_i
      when IR::Opcode::BR_GE_INT then b.as_i >= c.as_i
      when IR::Opcode::BR_EQ_INT then b.as_i == c.as_i
      when IR::Opcode::BR_NE_INT then b.as_i != c.as_i
      else
        raise VoltRuntimeError.new( "unhandled branch opcode #{op}" )
      end
    end

    private def eval_br_cond_imm( op : IR::Opcode, b : IR::Value, imm : Int32 ) : Bool
      case op
      when IR::Opcode::BR_LT_INT_IMM then b.as_i < imm.to_i64
      when IR::Opcode::BR_LE_INT_IMM then b.as_i <= imm.to_i64
      when IR::Opcode::BR_GT_INT_IMM then b.as_i > imm.to_i64
      when IR::Opcode::BR_GE_INT_IMM then b.as_i >= imm.to_i64
      when IR::Opcode::BR_EQ_INT_IMM then b.as_i == imm.to_i64
      when IR::Opcode::BR_NE_INT_IMM then b.as_i != imm.to_i64
      else
        raise VoltRuntimeError.new( "unhandled branch-imm opcode #{op}" )
      end
    end

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
