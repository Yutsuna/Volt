require "./Frame"
require "./Dispatch/__all__"
require "./Native/DispatchBinding"


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

    # Upper bound on Volt call depth threaded through the C core's shared frame
    # buffer. A deeper recursion is reported as a stack overflow (it would blow
    # the register stack anyway). 64K frames × 12 bytes = 768 KB per `execute`.
    FRAME_CAP = 1 << 16

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

      # Flat C-readable chunk metadata table for the direct-threaded core,
      # rebuilt lazily when the chunk set changes (see `ensure_chunk_table`).
      @chunk_table      = Pointer( LibVoltDispatch::ChunkInfo ).null
      @chunk_table_size = 0
      @chunk_index_of   = {} of UInt64 => Int32
      @chunk_raii_keep  = [] of Bytes   # keeps raii-reg buffers alive for the GC
      @chunk_feedback_keep = [] of Pointer( LibVoltDispatch::CallFeedback )

      # Stable FRClass heap boxes and flat class_index for FFI
      @class_boxes         = {} of Int32 => Pointer( LibVoltDispatch::RClass )
      @class_index         = Pointer( Pointer( LibVoltDispatch::RClass ) ).null
      @class_index_size    = 0
      @class_vtable_keep   = [] of Array( Int32 )
      @class_needs_rebuild = true
      # `-2` = not yet looked up, `-1` = looked up, no Core `String` class
      # registered (bare-unit specs) — resolved once, lazily, on first use.
      @string_type_id = -2
    end



    # Human-readable rendering of any `Value`, `String`-class instances
    # included : `Value#to_display`'s generic `Tag::Object` arm only knows
    # `<object:id>` (it can't see method bodies), so a raised/REPL-printed
    # `String` needs its bytes read directly off the known field layout
    # (`ptr`, `size`, `owned` — slots 0/1/2, `Core/String.vl`'s declaration
    # order) rather than calling back into `to_string` (this may run from
    # inside `RAISE`/`unwind`, not a safe place to re-enter `execute`).
    def display_value( v : IR::Value ) : String
      volt_string( v ) || v.to_display
    end

    # Reads a `Value` as a Crystal `String` if it's a real Core `String`-class
    # instance (`Tag::Object` whose `type_id` matches the resolved "String"
    # class) — its bytes are read directly off the known field layout :
    # `ptr`, `size`, `owned`, slots 0/1/2 per `Core/String.vl`'s declaration
    # order. `nil` for anything else (an ordinary object, a Regex, ...).
    def volt_string( v : IR::Value ) : String?
      return nil unless v.tag.object?
      obj = v.as_object
      if @string_type_id == -2
        @string_type_id = @registry.find_by_name( "String" ).try( &.type_id ) || -1
      end
      return nil unless obj.type_id == @string_type_id
      fields = obj.fields
      ptr    = fields[0].as_ptr.as( UInt8* )
      size   = fields[1].as_i.to_i32
      String.new( ptr, size )
    end

    #--------------------------------------------------------------------------

    # Builds the `FChunkInfo[]` the C core indexes on CALL, plus the
    # object-id → index map the Crystal driver needs to enter a chunk by object.
    # Idempotent: rebuilt only when `@unit.chunks` grows (REPL / `extend`).
    private def ensure_chunk_table : Nil
      return if @chunk_table_size == @unit.chunks.size && !@chunk_table.null?
      build_chunk_table
    end

    private def build_chunk_table : Nil
      n   = @unit.chunks.size
      tbl = Pointer( LibVoltDispatch::ChunkInfo ).malloc( n.to_u64 )
      keep     = Array( Bytes ).new( n )
      feedback_keep = Array( Pointer( LibVoltDispatch::CallFeedback ) ).new( n )
      index_of = {} of UInt64 => Int32

      @unit.chunks.each_with_index do |c, i|
        raii = c.raii_regs
        buf  = Bytes.new( raii.size )
        raii.each_with_index { |r, k| buf[ k ] = r.to_u8 }
        keep << buf

        fb_size = c.code.size
        if fb_size > 0
          fb = Pointer( LibVoltDispatch::CallFeedback ).malloc( fb_size )
          fb_size.times do |k|
            fb[ k ] = LibVoltDispatch::CallFeedback.new( last_class_id: -1, hit_count: 0 )
          end
        else
          fb = Pointer( LibVoltDispatch::CallFeedback ).null
        end
        feedback_keep << fb

        info = LibVoltDispatch::ChunkInfo.new
        info.code            = c.code.to_unsafe.as( Void* )
        info.code_size       = c.code.size
        info.consts          = c.constants.to_unsafe.as( Void* )
        info.num_registers   = c.num_registers
        info.raii_regs       = buf.to_unsafe.as( Void* )
        info.raii_regs_count  = raii.size
        info.has_drop_map    = c.drop_map.empty? ? 0_u8 : 1_u8
        info.feedback        = fb.as( Void* )
        tbl[ i ] = info

        index_of[ c.object_id ] = i
      end

      @chunk_table         = tbl
      @chunk_table_size    = n
      @chunk_index_of      = index_of
      @chunk_raii_keep     = keep
      @chunk_feedback_keep = feedback_keep
    end

    private def ensure_class_table : Nil
      return if !@class_needs_rebuild && !@class_index.null?
      build_class_table
    end

    private def build_class_table : Nil
      max_id = -1
      @unit.classes.each do |c|
        max_id = c.type_id if c.type_id > max_id
      end

      n = max_id + 1
      n = 0 if n < 0

      idx = Pointer( Pointer( LibVoltDispatch::RClass ) ).malloc( n ) { Pointer( LibVoltDispatch::RClass ).null }
      vtable_keep = [] of Array( Int32 )

      @unit.classes.each do |c|
        box = @class_boxes[ c.type_id ]?
        if box.nil?
          box = Pointer( LibVoltDispatch::RClass ).malloc( 1_u64 )
          @class_boxes[ c.type_id ] = box
        end

        vtable_keep << c.vtable

        box.value.type_id     = c.type_id
        box.value.slot_count  = c.slot_count
        box.value.dtor_index  = c.dtor_index
        box.value.vtable_size = c.vtable.size
        box.value.vtable      = c.vtable.to_unsafe

        idx[ c.type_id ] = box
      end

      @class_index          = idx
      @class_index_size     = n
      @class_vtable_keep    = vtable_keep
      @class_needs_rebuild  = false
    end

    #--------------------------------------------------------------------------

    def extend( unit : Compiler::Unit ) : Nil
      @class_needs_rebuild = true
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

    def allocate_object_for_c( type_id : Int32, out_val : IR::Value* ) : Nil
      slots = @registry[ type_id ]?.try( &.slot_count ) || 0
      obj = IR::HeapObject.allocate( type_id, slots )
      if box = @class_boxes[ type_id ]?
        obj.class_ref = box.as( Void* )
      end
      out_val.value = IR::Value.object( obj )
    end

    def call_chunk_at( index : Int32, args : Array( IR::Value ) = [] of IR::Value ) : Array( IR::Value )
      call_chunk( @unit.chunks[ index ], args )
    end

    #--------------------------------------------------------------------------

    def run : Int32
      execute_index( @unit.main_index, 1 )
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
      ensure_chunk_table
      idx = @chunk_index_of[ chunk.object_id ]?
      raise VoltRuntimeError.new( "chunk '#{chunk.name}' is not part of this unit" ) unless idx
      base = @stack_top
      base = 1 if base < 1
      args.each_with_index { |a, i| @stack[ base + i ] = a }
      n = execute_index( idx, base )
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

    # ── LIVE PATH ─────────────────────────────────────────────────────────
    #
    # Interpret the chunk at `chunk_index` in the register window at `base` by
    # driving the C direct-threaded core (Source/C/Dispatch.c). The core owns
    # the hot loop (arith, comparisons, branches, CALL/RET) and returns only to
    # let Crystal service a cold opcode (VM_CALLBACK), halt (VM_HALT), or raise
    # (VM_ERR_*). Return/results contract is identical to `execute_reference`:
    # results are written to `@stack[base - 1 ..]` and the slot count returned.
    private def execute_index( chunk_index : Int32, base : Int32 ) : Int32
      ensure_chunk_table
      ensure_class_table
      chunk     = @unit.chunks[ chunk_index ]
      saved_top = @stack_top
      @stack_top = base + chunk.num_registers

      # Nil-init the initial frame's recycled RAII registers (the C core and the
      # CALL_METHOD cold handler nil-init callee frames themselves).
      regs = @stack + base
      chunk.raii_regs.each { |r| regs[ r ] = IR::Value.nil_value }

      # Per-invocation frame buffer (matches the re-entrant `frames` Array of the
      # reference loop: a nested `call_chunk` gets its own).
      frames_buf = Pointer( LibVoltDispatch::CallFrame ).malloc( FRAME_CAP.to_u64 )

      ctx = LibVoltDispatch::VmContext.new
      ctx.chunks         = @chunk_table.as( Void* )
      ctx.chunk_count    = @unit.chunks.size
      ctx.stack          = @stack.as( Void* )
      ctx.stack_capacity = STACK_CAPACITY
      ctx.globals        = @globals.to_unsafe.as( Void* )
      ctx.global_count   = @globals.size
      ctx.frames         = frames_buf.as( Void* )
      ctx.frame_cap      = FRAME_CAP
      ctx.cur_chunk      = chunk_index
      ctx.base           = base
      ctx.ip             = 0
      ctx.frame_depth    = 0
      ctx.stack_top      = @stack_top
      ctx.class_index    = @class_index.as( Void* )
      ctx.class_count    = @class_index_size
      ctx.user_data      = Pointer( Void ).new( self.object_id )
      ctx.alloc_object   = ->( user_data : Void*, type_id : Int32, out_val : Void* ) {
        vm = user_data.unsafe_as( Vm )
        vm.allocate_object_for_c( type_id, out_val.as( IR::Value* ) )
        nil
      }

      begin
        while true
          status = LibVoltDispatch.dispatch( pointerof( ctx ) )
          case status
          when DispatchStatus::HALT
            return ctx.result_slots
          when DispatchStatus::CALLBACK
            halt = handle_cold( pointerof( ctx ), frames_buf )
            return halt if halt
          when DispatchStatus::ERR_DIVZERO
            raise DivisionByZeroError.new
          when DispatchStatus::ERR_OVERFLOW
            raise OverflowError.new
          when DispatchStatus::ERR_STACKOVER
            raise VoltRuntimeError.new( "stack overflow" )
          when DispatchStatus::ERR_NILRECEIVER
            raise VoltRuntimeError.new( "nil receiver" )
          when DispatchStatus::ERR_NOMETHOD
            ins  = IR::Instruction.new( ctx.stack.as( UInt32* )[ ctx.base + ctx.ip ] )
            recv = ctx.stack.as( IR::Value* )[ ctx.base + ins.a + 1 ]
            obj  = recv.as_object
            rclass = @registry[ obj.type_id ]?
            if rclass
               raise VoltRuntimeError.new( "unresolved virtual method (vtable slot #{ins.b}) on #{rclass.name}" )
            else
               raise VoltRuntimeError.new( "unknown class for type_id #{obj.type_id}" )
            end
          when DispatchStatus::ERR_NULLPTR
            raise VoltRuntimeError.new( "null pointer exception" )
          when DispatchStatus::ERR_BADOP
            raise VoltRuntimeError.new( "opcode #{IR::Opcode.new( ctx.cold_op.to_u8 )} is not yet implemented" )
          else
            raise VoltRuntimeError.new( "unknown dispatch status #{status}" )
          end
        end
      rescue ex
        # Exception unwind: run RAII drops for the faulting frame and every
        # suspended caller, deepest first (mirrors the reference loop's rescue).
        unwind_all( ctx, frames_buf )
        raise ex
      ensure
        @stack_top = saved_top
      end
    end

    #--------------------------------------------------------------------------

    # Services one cold opcode at the `ctx` cursor, mutating `ctx` to advance.
    # Returns a non-nil slot count only when a RAII-bearing RET unwinds the
    # outermost frame (halt); otherwise nil to keep driving the loop.
    private def handle_cold( ctx : LibVoltDispatch::VmContext*, frames_buf : Pointer( LibVoltDispatch::CallFrame ) ) : Int32?
      # Keep a re-entrant `call_chunk` (e.g. from a `finalize` during DROP)
      # windowed above this frame.
      @stack_top = ctx.value.stack_top

      chunk = @unit.chunks[ ctx.value.cur_chunk ]
      ip    = ctx.value.ip
      base  = ctx.value.base
      ins   = chunk.code[ ip ]
      op    = ins.op
      frame = Frame.new( @stack + base )

      case op
      when .call_method?
        return handle_call_method( ctx, frames_buf )
      when .ret?
        return handle_ret_dropmap( ctx, frames_buf )
      when .call_native?
        frame[ ins.a ] = call_native( ins.b, collect_args( frame, ins ) )
      when .eq?, .ne?, .cmp_int?
        frame[ ins.a ] = eval_cmp( op, frame[ ins.b ], frame[ ins.c ] )
      when .shl_int?, .shr_int?, .pow_int?
        frame[ ins.a ] = eval_arith( op, frame[ ins.b ], frame[ ins.c ] )
      when .raise?
        raise VoltRuntimeError.new( display_value( frame[ ins.a ] ) )
      when .init?, .drop?, .drop_scope?
        exec_raii( frame, chunk, ins )
      when .init_obj?, .load_field?, .store_field?, .copy_block?, .new_struct?
        exec_object( frame, chunk, ins )
      else
        raise VoltRuntimeError.new( "opcode #{op} is not yet implemented" )
      end

      # Every serviced cold opcode is single-slot: advance past it.
      c = ctx.value
      c.ip = ip + 1
      ctx.value = c
      nil
    end

    # CALL_METHOD serviced in Crystal (needs the class registry / vtable), then
    # the callee frame is pushed onto the shared C frame buffer so the core
    # resumes inside it. Mirrors the reference loop's CALL_METHOD arm.
    private def handle_call_method( ctx : LibVoltDispatch::VmContext*, frames_buf : Pointer( LibVoltDispatch::CallFrame ) ) : Int32?
      c     = ctx.value
      chunk = @unit.chunks[ c.cur_chunk ]
      ins   = chunk.code[ c.ip ]

      obj    = @stack[ c.base + ins.a + 1 ].as_object
      rclass = @registry[ obj.type_id ]?
      raise VoltRuntimeError.new( "unknown class for type_id #{obj.type_id}" ) unless rclass
      chunk_idx = rclass.vtable[ ins.b ]?
      if chunk_idx.nil? || chunk_idx < 0
        raise VoltRuntimeError.new( "unresolved virtual method (vtable slot #{ins.b}) on #{rclass.name}" )
      end

      callee = @unit.chunks[ chunk_idx ]
      cbase  = c.base + ins.a + 1
      if cbase + callee.num_registers > STACK_CAPACITY || c.frame_depth >= FRAME_CAP
        raise VoltRuntimeError.new( "stack overflow" )
      end

      resume = LibVoltDispatch::CallFrame.new
      resume.chunk_index      = c.cur_chunk
      resume.base             = c.base
      resume.ip               = c.ip + 1
      frames_buf[ c.frame_depth ] = resume
      c.frame_depth += 1

      c.cur_chunk = chunk_idx
      c.base      = cbase
      c.ip        = 0
      c.stack_top = cbase + callee.num_registers
      ctx.value   = c
      @stack_top  = c.stack_top

      regs = @stack + cbase
      callee.raii_regs.each { |r| regs[ r ] = IR::Value.nil_value }
      nil
    end

    # RET bounced from the core because the returning chunk has live RAII
    # objects to drop on an early return. Copies results, runs the drops, then
    # pops the caller (or halts if outermost). Mirrors the reference RET arm.
    private def handle_ret_dropmap( ctx : LibVoltDispatch::VmContext*, frames_buf : Pointer( LibVoltDispatch::CallFrame ) ) : Int32?
      c     = ctx.value
      chunk = @unit.chunks[ c.cur_chunk ]
      ins   = chunk.code[ c.ip ]
      base  = c.base
      slots = ins.bx > 0 ? ins.bx : 1

      j = 0
      while j < slots
        @stack[ base - 1 + j ] = @stack[ base + ins.a + j ]
        j += 1
      end

      resume_ip = c.ip + 1
      unwind_frame( Frame.new( @stack + base ), chunk, resume_ip ) if resume_ip < chunk.code.size && !chunk.drop_map.empty?

      if c.frame_depth == 0
        ctx.value = c
        return slots
      end

      c.frame_depth -= 1
      resumed     = frames_buf[ c.frame_depth ]
      c.cur_chunk = resumed.chunk_index
      c.base      = resumed.base
      c.ip        = resumed.ip
      c.stack_top = resumed.base + @unit.chunks[ resumed.chunk_index ].num_registers
      ctx.value   = c
      @stack_top  = c.stack_top
      nil
    end

    # Walks the current frame and every suspended caller (deepest first),
    # running RAII drops for any whose DropMap covers its live pc. Used on the
    # exception path, identical in effect to the reference loop's rescue.
    private def unwind_all( ctx : LibVoltDispatch::VmContext, frames_buf : Pointer( LibVoltDispatch::CallFrame ) ) : Nil
      cur = @unit.chunks[ ctx.cur_chunk ]
      if ctx.ip < cur.code.size && !cur.drop_map.empty?
        unwind_frame( Frame.new( @stack + ctx.base ), cur, ctx.ip )
      end
      d = ctx.frame_depth - 1
      while d >= 0
        f  = frames_buf[ d ]
        sc = @unit.chunks[ f.chunk_index ]
        if f.ip < sc.code.size && !sc.drop_map.empty?
          unwind_frame( Frame.new( @stack + f.base ), sc, f.ip )
        end
        d -= 1
      end
    end

    #--------------------------------------------------------------------------

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
    #
    # RETAINED as the pure-Crystal reference implementation (fallback + A/B
    # correctness oracle). The live path is `execute_index`, which hands the hot
    # loop to the C direct-threaded core (Source/C/Dispatch.c).
    private def execute_reference( chunk : IR::Chunk, base : Int32 ) : Int32
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

            when IR::Opcode::RAISE
              raise VoltRuntimeError.new( display_value( regs[ins.a] ) )

            # ---- cold / rare : delegate to family handlers ----
            when IR::Opcode::CONV_INT
              exec_conv_int( frame, ins )

            when IR::Opcode::CALL_NATIVE
              regs[ins.a] = call_native( ins.b, collect_args( frame, ins ) )

            when IR::Opcode::CMP_INT
              regs[ins.a] = eval_cmp( op, regs[ins.b], regs[ins.c] )

            when IR::Opcode::INIT, IR::Opcode::DROP, IR::Opcode::DROP_SCOPE
              exec_raii( frame, chunk, ins )

            when IR::Opcode::INIT_OBJ, IR::Opcode::LOAD_FIELD, IR::Opcode::STORE_FIELD,
                 IR::Opcode::COPY_BLOCK, IR::Opcode::NEW_STRUCT
              exec_object( frame, chunk, ins )

            when IR::Opcode::LOAD_PTR, IR::Opcode::STORE_PTR, IR::Opcode::PTR_ADD,
                 IR::Opcode::PTR_SUB, IR::Opcode::ADDR_LOCAL, IR::Opcode::ADDR_FIELD
              exec_pointer( frame, chunk, ins )

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
