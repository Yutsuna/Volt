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
    CALL          # A,B,C  : reg[A] = chunks[B]( reg[A+1 .. A+C] )
    CALL_NATIVE   # A,B,C  : reg[A] = native[B]( reg[A+1 .. A+C] )
    RET           # A      : return reg[A]
  end


end
