module Volt::IR


  # Typed Tier-0 opcodes. The static type proven by the frontend tells the compiler
  # which variant to emit, so the VM never reads a tag before acting (architecture #5).
  #
  # Encoding (see Instruction): most use the A/B/C register form; LOAD_CONST and the
  # jumps use the A/Bx (16-bit operand) form.
  enum Opcode : UInt8
    # --- load / move -------------------------------------------------
    LOAD_CONST    # A, Bx : reg[A] = constants[Bx]
    LOAD_TRUE     # A      : reg[A] = true
    LOAD_FALSE    # A      : reg[A] = false
    LOAD_NIL      # A      : reg[A] = nil
    MOVE          # A, B   : reg[A] = reg[B]

    # --- integer arithmetic -----------------------------------------
    ADD_INT       # A,B,C  : reg[A] = reg[B] + reg[C]
    SUB_INT
    MUL_INT
    DIV_INT
    MOD_INT
    NEG_INT       # A,B    : reg[A] = -reg[B]

    # --- float arithmetic -------------------------------------------
    ADD_F64
    SUB_F64
    MUL_F64
    DIV_F64
    NEG_F64

    # --- comparison (result is Bool) --------------------------------
    LT_INT
    LE_INT
    GT_INT
    GE_INT
    LT_F64
    LE_F64
    GT_F64
    GE_F64
    EQ            # A,B,C  : reg[A] = reg[B] == reg[C]   (value equality, any type)
    NE
    EQ_INT        # A,B,C  : reg[A] = reg[B] == reg[C]   (typed : both operands proven Int)
    NE_INT

    # --- logical -----------------------------------------------------
    NOT           # A,B    : reg[A] = !truthy?(reg[B])

    # --- control flow ------------------------------------------------
    JMP           # Bx     : ip = Bx
    JMP_IF_FALSE  # A, Bx  : if !truthy?(reg[A]) then ip = Bx

    # --- calls -------------------------------------------------------
    # `C` on CALL/CALL_NATIVE and `Bx` on RET are *slot* counts, not logical
    # argument/return-value counts : a struct-typed argument or return value
    # flattens into N contiguous slots (architecture #1.B), so a multi-slot
    # struct is marshaled as a positional run of registers, same as N scalar
    # arguments would be.
    CALL          # A,B,C  : reg[A .. A+?] = chunks[B]( reg[A+1 .. A+C] )
    CALL_NATIVE   # A,B,C  : reg[A] = native[B]( reg[A+1 .. A+C] )
    RET           # A, Bx  : return reg[A .. A+Bx-1]   (Bx = slot count, default 1)

    # --- bitwise ---
    AND_INT       # A,B,C : reg[A] = reg[B] & reg[C]
    OR_INT        # A,B,C : reg[A] = reg[B] | reg[C]
    XOR_INT       # A,B,C : reg[A] = reg[B] ^ reg[C]
    SHL_INT       # A,B,C : reg[A] = reg[B] << reg[C]
    SHR_INT       # A,B,C : reg[A] = reg[B] >> reg[C]
    NOT_INT       # A,B   : reg[A] = ~reg[B]

    # --- integer division ---
    IDIV_INT      # A,B,C : reg[A] = reg[B] // reg[C]

    # --- power ---
    POW_INT       # A,B,C : reg[A] = reg[B] ** reg[C]

    # --- wrapping conversion ---
    CONV_INT      # A,B   : reg[A] = sign_extend(reg[A], B bits)

    # --- spaceship ---
    CMP_INT       # A,B,C : reg[A] = (reg[B] <=> reg[C])



    # --- memory RAII ---
    INIT          # A, Bx : reg[A] = allocate(Bx)
    DROP          # A, Bx : deallocate(reg[A], Bx)
    DROP_SCOPE    # Bx    : deallocate_scope(regs_idx: Bx)

    # --- raise ---
    RAISE         # A     : raise reg[A]

    # --- object model (Phase 1/2: classes/structs) ---
    #
    # Fields are addressed by *register slot*, not byte offset: every class
    # instance carries a heap `fields : Array(Value)` and every struct value
    # lives directly in a contiguous run of Frame registers (architecture #1.B
    # : no allocation, no packing/alignment at this tier). A field's slot
    # index comes from `TypeInfo#reg_layout` (Phase B), where a primitive or
    # class-reference field is exactly one slot and a nested struct field
    # flattens in as N slots (N = that struct's own slot count). Because both
    # representations are just "a Value per slot", one opcode pair covers
    # both: local struct field reads/writes compile straight to `MOVE`
    # against `base + slot`, while `LOAD_FIELD`/`STORE_FIELD` cover class
    # instances (and struct sub-fields once boxed into a class's own array).
    INIT_OBJ      # A, Bx : reg[A] = alloc_object(type_id: Bx) : fields zero/nil-init, vtable wired
    LOAD_FIELD    # A,B,C : reg[A] = obj(reg[B]).fields[C]
    STORE_FIELD   # A,B,C : obj(reg[A]).fields[B] = reg[C]

    # --- object dispatch ---
    # Same calling convention as `CALL`/`CALL_NATIVE` (A is both the return
    # landing slot and `window_start - 1` ; `collect_args` reads C slots from
    # `A+1..`) plus one extra piece : B is the vtable slot index, resolved
    # statically from the receiver's *static* type (`TypeInfo#vtable_layout`)
    # at compile time. The receiver itself travels as the first (always
    # 1-slot, since class refs never flatten) argument at `A+1` — the VM
    # reads `frame[A+1].as_object.type_id`, looks that up in the class
    # registry, and dispatches through *that* class's own `vtable[B]` :
    # the slot index is shared across the whole hierarchy (architecture
    # #7.3), so a `Device`-typed call site correctly lands on `UsbDrive`'s
    # override at runtime.
    CALL_METHOD   # A,B,C : reg[A] = vtable_call(vtidx: B, args: reg[A+1 .. A+C])
    CALL_MIXIN    # A,Bx  : reg[A] = itable_call(chunk.call_sites[Bx], args: window)  -- reserved, unused

    # --- struct value semantics ---
    COPY_BLOCK    # A,B,C : reg[A .. A+C-1] = reg[B .. B+C-1]   (register-range copy, C = slot count)
    NEW_STRUCT    # A,Bx  : reg[A .. A+Bx-1] = nil   (reserve/zero a Bx-slot struct block)

    # --- module class-variables (@@name) ---
    # Process-global storage : a module has no instances, so its `@@vars` live
    # in one flat VM-wide array indexed by a compile-assigned slot (`Bx`).
    LOAD_GLOBAL   # A,Bx  : reg[A]      = globals[Bx]
    STORE_GLOBAL  # A,Bx  : globals[Bx] = reg[A]



    # --- peephole superinstructions (Compiler::Peephole, architecture #9) ---
    #
    # Emitted only by the peephole pass, never by FunctionEmiter directly.
    # Every fusion is *instruction-count-preserving* : the slot(s) it removes
    # become `NOP`, so `JMP`/`JMP_IF_FALSE` targets and `DropMap` pc ranges
    # never need remapping.
    NOP           #        : no-op (peephole padding)

    # 8-bit unsigned immediate forms : reg[C] is a literal 0..255, not a
    # register. Fused from `[LOAD_CONST t,k][op a,b,t]` when `k` fits.
    ADD_INT_IMM   # A,B,C  : reg[A] = reg[B] + C
    SUB_INT_IMM   # A,B,C  : reg[A] = reg[B] - C
    EQ_INT_IMM    # A,B,C  : reg[A] = reg[B] == C
    NE_INT_IMM    # A,B,C  : reg[A] = reg[B] != C

    # Compare+branch fusion : fused from `[LT/LE/GT/GE/EQ/NE_INT d,B,C][JMP_IF_FALSE d,_]`.
    # `A` is unused. The jump target is *not* encoded in this instruction: the
    # following code slot is left as the original (untouched) `JMP_IF_FALSE`,
    # whose `Bx` the VM reads directly for the target -- so no boolean `Value`
    # is ever materialised for the branch. B,C : compared registers.
    BR_LT_INT
    BR_LE_INT
    BR_GT_INT
    BR_GE_INT
    BR_EQ_INT
    BR_NE_INT

    # Same, but C is an 8-bit unsigned immediate instead of a register.
    # Fused from `[LOAD_CONST t,k][CMP_INT d,B,t][JMP_IF_FALSE d,_]`.
    BR_LT_INT_IMM
    BR_LE_INT_IMM
    BR_GT_INT_IMM
    BR_GE_INT_IMM
    BR_EQ_INT_IMM
    BR_NE_INT_IMM

    # --- pointer & memory operations ---
    LOAD_PTR      # A,B,C : reg[A] = *(reg[B].ptr)  (C = width)
    STORE_PTR     # A,B,C : *(reg[A].ptr) = reg[B]  (C = width)
    PTR_ADD       # A,B,C : reg[A] = reg[B].ptr + reg[C].offset
    PTR_SUB       # A,B,C : reg[A] = reg[B].ptr - reg[C].offset
    ADDR_LOCAL    # A,B   : reg[A] = &Stack[Base+B].Payload
    ADDR_FIELD    # A,B,C : reg[A] = &obj(reg[B]).Fields[C].Payload
  end


end
