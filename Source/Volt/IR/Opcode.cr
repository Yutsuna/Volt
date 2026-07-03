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

    # --- regex match ---
    MATCH_STR     # A,B,C : reg[A] = reg[B] =~ reg[C]
    NOT_MATCH_STR # A,B,C : reg[A] = !(reg[B] =~ reg[C])

    # --- triple equal ---
    EQ_CASE       # A,B,C : reg[A] = reg[B] === reg[C]

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

    # --- minimal string builtins ---
    TO_STRING     # A,B   : reg[A] = reg[B].to_display   (Int/Float/Bool/... -> String)
    CONCAT_STR    # A,B,C : reg[A] = reg[B] + reg[C]      (String concatenation)
  end


end
