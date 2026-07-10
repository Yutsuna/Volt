module Volt::Compiler


  # Tier-0 peephole superinstructions (architecture #9). Three independent
  # forward scans per chunk, each rewriting `chunk.code` **in place** without
  # changing its length: every fusion turns a slot it no longer needs into
  # `IR::Opcode::NOP` rather than deleting it, so `JMP`/`JMP_IF_FALSE` targets
  # and `DropMap` pc ranges (both absolute instruction indices) stay valid
  # with zero remapping.
  #
  # Safety for all three passes rests on one property of `FunctionEmiter`'s
  # register allocator: `alloc`/`alloc_block` only ever increment `@next_reg`
  # -- a register is written by exactly one instruction in the whole chunk.
  # So "is this temp dead after its one known consumer" reduces to a single
  # linear read-count scan, no full dataflow analysis needed.
  module Peephole

    NOP = IR::Instruction.abc( IR::Opcode::NOP, 0, 0, 0 )

    # Opcodes that write a single result register in `A`, safe to redirect to
    # a different destination register (used by move-elimination). Excludes
    # anything with implicit multi-slot results (CALL*, COPY_BLOCK, RET) or
    # that reads its own `A` as an input (CONV_INT).
    SINGLE_DEST_OPS = {
      IR::Opcode::LOAD_CONST, IR::Opcode::LOAD_TRUE, IR::Opcode::LOAD_FALSE, IR::Opcode::LOAD_NIL,
      IR::Opcode::ADD_INT, IR::Opcode::SUB_INT, IR::Opcode::MUL_INT, IR::Opcode::DIV_INT,
      IR::Opcode::MOD_INT, IR::Opcode::NEG_INT, IR::Opcode::ADD_F64, IR::Opcode::SUB_F64,
      IR::Opcode::MUL_F64, IR::Opcode::DIV_F64, IR::Opcode::NEG_F64, IR::Opcode::AND_INT,
      IR::Opcode::OR_INT, IR::Opcode::XOR_INT, IR::Opcode::SHL_INT, IR::Opcode::SHR_INT,
      IR::Opcode::NOT_INT, IR::Opcode::IDIV_INT, IR::Opcode::POW_INT, IR::Opcode::LT_INT,
      IR::Opcode::LE_INT, IR::Opcode::GT_INT, IR::Opcode::GE_INT, IR::Opcode::LT_F64,
      IR::Opcode::LE_F64, IR::Opcode::GT_F64, IR::Opcode::GE_F64, IR::Opcode::EQ, IR::Opcode::NE,
      IR::Opcode::EQ_INT, IR::Opcode::NE_INT, IR::Opcode::CMP_INT,
      IR::Opcode::NOT,
      IR::Opcode::LOAD_GLOBAL, IR::Opcode::LOAD_FIELD, IR::Opcode::INIT_OBJ,
      IR::Opcode::ADD_INT_IMM, IR::Opcode::SUB_INT_IMM, IR::Opcode::EQ_INT_IMM, IR::Opcode::NE_INT_IMM,
    }

    # Instruction format: `Bx` (16-bit operand) vs `A,B,C` (three 8-bit regs).
    ABX_FORM = { IR::Opcode::LOAD_CONST, IR::Opcode::LOAD_GLOBAL, IR::Opcode::INIT_OBJ }

    INT_CMP_OPS = {
      IR::Opcode::LT_INT => IR::Opcode::BR_LT_INT, IR::Opcode::LE_INT => IR::Opcode::BR_LE_INT,
      IR::Opcode::GT_INT => IR::Opcode::BR_GT_INT, IR::Opcode::GE_INT => IR::Opcode::BR_GE_INT,
      IR::Opcode::EQ_INT => IR::Opcode::BR_EQ_INT, IR::Opcode::NE_INT => IR::Opcode::BR_NE_INT,
    }

    INT_CMP_IMM_OPS = {
      IR::Opcode::LT_INT => IR::Opcode::BR_LT_INT_IMM, IR::Opcode::LE_INT => IR::Opcode::BR_LE_INT_IMM,
      IR::Opcode::GT_INT => IR::Opcode::BR_GT_INT_IMM, IR::Opcode::GE_INT => IR::Opcode::BR_GE_INT_IMM,
      IR::Opcode::EQ_INT => IR::Opcode::BR_EQ_INT_IMM, IR::Opcode::NE_INT => IR::Opcode::BR_NE_INT_IMM,
    }

    IMM_ARITH_OPS = {
      IR::Opcode::ADD_INT => IR::Opcode::ADD_INT_IMM, IR::Opcode::SUB_INT => IR::Opcode::SUB_INT_IMM,
      IR::Opcode::EQ_INT  => IR::Opcode::EQ_INT_IMM,  IR::Opcode::NE_INT  => IR::Opcode::NE_INT_IMM,
    }
    # Which of these are commutative (so a left-operand-immediate also fuses).
    IMM_ARITH_COMMUTATIVE = { IR::Opcode::ADD_INT, IR::Opcode::EQ_INT, IR::Opcode::NE_INT }

    def self.run( unit : Unit ) : Unit
      unit.chunks.each { |chunk| run_chunk( chunk ) }
      unit
    end

    private def self.run_chunk( chunk : IR::Chunk ) : Nil
      fuse_compare_branch( chunk )
      fuse_immediate_arith( chunk )
      eliminate_moves( chunk )
    end

    #--------------------------------------------------------------------------
    # Register read-count, computed fresh from current chunk.code. Needed by
    # all three passes to prove a temp register has exactly one reader (the
    # instruction we're about to fuse it into) before repurposing/removing it.

    private def self.read_counts( chunk : IR::Chunk ) : Hash( Int32, Int32 )
      counts = Hash( Int32, Int32 ).new( 0 )
      chunk.code.each do |ins|
        reads_of( chunk, ins ).each { |r| counts[ r ] += 1 }
      end
      counts
    end

    private def self.reads_of( chunk : IR::Chunk, ins : IR::Instruction ) : Array( Int32 )
      case ins.op
      when IR::Opcode::MOVE, IR::Opcode::NOT, IR::Opcode::NEG_INT, IR::Opcode::NEG_F64,
           IR::Opcode::NOT_INT, IR::Opcode::LOAD_FIELD
        [ ins.b ]
      when IR::Opcode::ADD_INT, IR::Opcode::SUB_INT, IR::Opcode::MUL_INT, IR::Opcode::DIV_INT,
           IR::Opcode::MOD_INT, IR::Opcode::ADD_F64, IR::Opcode::SUB_F64, IR::Opcode::MUL_F64,
           IR::Opcode::DIV_F64, IR::Opcode::AND_INT, IR::Opcode::OR_INT, IR::Opcode::XOR_INT,
           IR::Opcode::SHL_INT, IR::Opcode::SHR_INT, IR::Opcode::IDIV_INT, IR::Opcode::POW_INT,
           IR::Opcode::LT_INT, IR::Opcode::LE_INT, IR::Opcode::GT_INT, IR::Opcode::GE_INT,
           IR::Opcode::LT_F64, IR::Opcode::LE_F64, IR::Opcode::GT_F64, IR::Opcode::GE_F64,
           IR::Opcode::EQ, IR::Opcode::NE, IR::Opcode::EQ_INT, IR::Opcode::NE_INT,
           IR::Opcode::CMP_INT,
           IR::Opcode::ADD_INT_IMM, IR::Opcode::SUB_INT_IMM,
           IR::Opcode::EQ_INT_IMM, IR::Opcode::NE_INT_IMM,
           IR::Opcode::BR_LT_INT, IR::Opcode::BR_LE_INT, IR::Opcode::BR_GT_INT,
           IR::Opcode::BR_GE_INT, IR::Opcode::BR_EQ_INT, IR::Opcode::BR_NE_INT
        [ ins.b, ins.c ]
      when IR::Opcode::BR_LT_INT_IMM, IR::Opcode::BR_LE_INT_IMM, IR::Opcode::BR_GT_INT_IMM,
           IR::Opcode::BR_GE_INT_IMM, IR::Opcode::BR_EQ_INT_IMM, IR::Opcode::BR_NE_INT_IMM
        [ ins.b ]
      when IR::Opcode::JMP_IF_FALSE, IR::Opcode::RAISE, IR::Opcode::DROP, IR::Opcode::STORE_GLOBAL,
           IR::Opcode::CONV_INT
        [ ins.a ]
      when IR::Opcode::STORE_FIELD
        [ ins.a, ins.c ]
      when IR::Opcode::CALL, IR::Opcode::CALL_NATIVE, IR::Opcode::CALL_METHOD
        ( 1 .. ins.c ).map { |k| ins.a + k }
      when IR::Opcode::RET
        n = ins.bx > 0 ? ins.bx : 1
        ( 0 ... n ).map { |k| ins.a + k }
      when IR::Opcode::COPY_BLOCK
        ( 0 ... ins.c ).map { |k| ins.b + k }
      when IR::Opcode::DROP_SCOPE
        chunk.scope_tables[ ins.bx ]? || ( [] of Int32 )
      else
        [] of Int32
      end
    end

    #--------------------------------------------------------------------------
    # Pass 1 : compare + branch fusion.
    #
    #   [LT/LE/GT/GE/EQ/NE_INT d,B,C][JMP_IF_FALSE d,target]
    #     -> [BR_xx _,B,C][JMP_IF_FALSE d,target]   (2nd slot UNCHANGED,
    #        reused purely as a target holder -- the VM's BR_xx handler reads
    #        its `Bx` directly and never dispatches it as an instruction)
    #
    #   [LOAD_CONST t,k][LT/LE/GT/GE/EQ/NE_INT d,B,t or d,t,C][JMP_IF_FALSE d,target]
    #     -> [NOP][BR_xx_IMM _,B,k][JMP_IF_FALSE d,target]   (same idea, one
    #        more slot folded away; the immediate is baked into the branch)
    #
    # Safety: `d`'s only reader must be this `JMP_IF_FALSE` (else something
    # else needs the materialised bool -- can't skip producing it), and
    # nothing may jump directly into the `JMP_IF_FALSE` slot itself (that
    # would dispatch it as a real instruction, reading the now-uninitialised
    # `d`). The slot *before* the comparison (NOP or the original compare) has
    # no such hazard: it's always safe to land on since both replacements are
    # self-contained.

    private def self.fuse_compare_branch( chunk : IR::Chunk ) : Nil
      code    = chunk.code
      targets = jump_targets( code )
      counts  = read_counts( chunk )

      j = 1
      while j < code.size
        jif = code[ j ]
        j += 1
        next unless jif.op == IR::Opcode::JMP_IF_FALSE

        cmp_i = j - 2
        next if cmp_i < 0
        cmp = code[ cmp_i ]
        br_op = INT_CMP_OPS[ cmp.op ]?
        next unless br_op
        next unless jif.a == cmp.a
        next unless counts[ cmp.a ]? == 1
        next if targets.includes?( j - 1 ) # nothing may jump straight into the JMP_IF_FALSE slot

        # Try to fold a preceding `LOAD_CONST` into the branch as an immediate.
        const_i = cmp_i - 1
        if const_i >= 0 && code[ const_i ].op == IR::Opcode::LOAD_CONST
          lc  = code[ const_i ]
          imm = small_int_const( chunk, lc )
          if imm && counts[ lc.a ]? == 1 && ( cmp.b == lc.a ) != ( cmp.c == lc.a )
            reg = cmp.b == lc.a ? cmp.c : cmp.b
            code[ const_i ] = NOP
            code[ cmp_i ]   = IR::Instruction.abc( INT_CMP_IMM_OPS[ cmp.op ], 0, reg, imm )
            next
          end
        end

        code[ cmp_i ] = IR::Instruction.abc( br_op, 0, cmp.b, cmp.c )
      end
    end

    private def self.jump_targets( code : Array( IR::Instruction ) ) : Set( Int32 )
      targets = Set( Int32 ).new
      code.each do |ins|
        targets << ins.bx if ins.op == IR::Opcode::JMP || ins.op == IR::Opcode::JMP_IF_FALSE
      end
      targets
    end

    #--------------------------------------------------------------------------
    # Pass 2 : immediate-operand arithmetic/equality, for LOAD_CONST/op pairs
    # the branch-fusion pass above didn't already consume.
    #
    #   [LOAD_CONST t,k][ADD/SUB/EQ/NE_INT a,B,t or a,t,C]  (k in 0..255)
    #     -> [NOP][ADD/SUB/EQ_INT_IMM a,B,k]

    private def self.fuse_immediate_arith( chunk : IR::Chunk ) : Nil
      code   = chunk.code
      counts = read_counts( chunk )

      i = 0
      while i < code.size - 1
        lc = code[ i ]
        unless lc.op == IR::Opcode::LOAD_CONST
          i += 1
          next
        end

        imm = small_int_const( chunk, lc )
        unless imm && counts[ lc.a ]? == 1
          i += 1
          next
        end

        nxt   = code[ i + 1 ]
        imm_op = IMM_ARITH_OPS[ nxt.op ]?
        unless imm_op
          i += 1
          next
        end

        t = lc.a
        if nxt.c == t && nxt.b != t
          code[ i ]     = NOP
          code[ i + 1 ] = IR::Instruction.abc( imm_op, nxt.a, nxt.b, imm )
        elsif nxt.b == t && nxt.c != t && IMM_ARITH_COMMUTATIVE.includes?( nxt.op )
          code[ i ]     = NOP
          code[ i + 1 ] = IR::Instruction.abc( imm_op, nxt.a, nxt.c, imm )
        end

        i += 1
      end
    end

    #--------------------------------------------------------------------------
    # Pass 3 : MOVE elimination.
    #
    #   [op a=t,...][MOVE home,t]   (t read nowhere else, `op` a SINGLE_DEST op)
    #     -> [op a=home,...][NOP]
    #
    # Always eliminates a pure self-move `MOVE r,r` outright (no liveness
    # check needed -- it's an identity regardless of who else reads `r`).

    private def self.eliminate_moves( chunk : IR::Chunk ) : Nil
      code   = chunk.code
      counts = read_counts( chunk )

      i = 0
      while i < code.size - 1
        mv = code[ i + 1 ]
        unless mv.op == IR::Opcode::MOVE
          i += 1
          next
        end

        if mv.a == mv.b
          code[ i + 1 ] = NOP
          i += 1
          next
        end

        producer = code[ i ]
        unless SINGLE_DEST_OPS.includes?( producer.op ) && producer.a == mv.b && counts[ mv.b ]? == 1
          i += 1
          next
        end

        home = mv.a
        code[ i ] = ABX_FORM.includes?( producer.op ) ? IR::Instruction.abx( producer.op, home, producer.bx ) \
                                                        : IR::Instruction.abc( producer.op, home, producer.b, producer.c )
        code[ i + 1 ] = NOP
        i += 1
      end
    end

    #--------------------------------------------------------------------------

    private def self.small_int_const( chunk : IR::Chunk, load_const : IR::Instruction ) : Int32?
      const = chunk.constants[ load_const.bx ]?
      return nil unless const
      return nil unless const.tag == IR::Value::Tag::Int
      raw = const.as_i
      return nil unless raw >= 0 && raw <= 255
      raw.to_i32
    end

  end


end
