// Instructions.inl — the manifest of primitive machine operations.
//
// One row per family × operator, and nothing else: no target encoding, no
// `switch`, no C++ logic (rules/meta-first.md). A consumer defines whichever of
// the three macros it cares about, includes this file, and gets exactly the
// rows it asked for; a target maps the neutral opcode enums onto its own
// encoding in its own file, so adding a backend adds a *column*, never a copy
// of these rows.
//
// `a + b` on a primitive is an instruction, not a method body: when
// UnitCallees left the operator unresolved, the receiver's
// Primitive{ Spelling, Bits } is what selects the row (rules/core-ast.md,
// rules/zero-hardcode.md). The family comes from one character of the
// spelling — `"i32"` selects signed rows because it starts with `i`, not
// because anything here knows it was written `Int32`.
//
// The signed/unsigned split exists exactly where the machine has two
// instructions for one operator (SDiv/UDiv, SLt/ULt) and nowhere else: `Add` is
// written once per family rather than made conditional.
//
// Deliberately absent, because none of them is a table lookup:
//   - `and` / `or` short-circuit, so they are control flow;
//   - `not` / `!` on a one-bit value needs the operand's width, so it is
//     EUnaryOp::LogicalNot rather than a binary row;
//   - pointer `+` / `-` are heterogeneous and emit an address computation whose
//     stride is the pointee's size, not an arithmetic opcode.

#ifndef VOLT_BIN_OP
    #define VOLT_BIN_OP( Family, Token, Opcode )
    #define VOLT_BIN_OP_DEFAULTED
#endif

#ifndef VOLT_CMP_OP
    #define VOLT_CMP_OP( Family, Token, Predicate )
    #define VOLT_CMP_OP_DEFAULTED
#endif

#ifndef VOLT_UN_OP
    #define VOLT_UN_OP( Family, Token, Kind )
    #define VOLT_UN_OP_DEFAULTED
#endif

// --- Arithmetic and bitwise, signed integers -------------------------------
VOLT_BIN_OP( SInt, Plus, Add )
VOLT_BIN_OP( SInt, Minus, Sub )
VOLT_BIN_OP( SInt, Star, Mul )
VOLT_BIN_OP( SInt, Slash, SDiv )
VOLT_BIN_OP( SInt, Percent, SRem )
VOLT_BIN_OP( SInt, Amp, And )
VOLT_BIN_OP( SInt, Pipe, Or )
VOLT_BIN_OP( SInt, Caret, Xor )
VOLT_BIN_OP( SInt, Shl, Shl )
// Arithmetic shift: the sign bit is meaningful.
VOLT_BIN_OP( SInt, Shr, AShr )

// --- Arithmetic and bitwise, unsigned integers -----------------------------
VOLT_BIN_OP( UInt, Plus, Add )
VOLT_BIN_OP( UInt, Minus, Sub )
VOLT_BIN_OP( UInt, Star, Mul )
VOLT_BIN_OP( UInt, Slash, UDiv )
VOLT_BIN_OP( UInt, Percent, URem )
VOLT_BIN_OP( UInt, Amp, And )
VOLT_BIN_OP( UInt, Pipe, Or )
VOLT_BIN_OP( UInt, Caret, Xor )
VOLT_BIN_OP( UInt, Shl, Shl )
// Logical shift: there is no sign bit to preserve.
VOLT_BIN_OP( UInt, Shr, LShr )

// --- Arithmetic, floats ----------------------------------------------------
VOLT_BIN_OP( Float, Plus, FAdd )
VOLT_BIN_OP( Float, Minus, FSub )
VOLT_BIN_OP( Float, Star, FMul )
VOLT_BIN_OP( Float, Slash, FDiv )
VOLT_BIN_OP( Float, Percent, FRem )

// --- Comparisons, signed integers ------------------------------------------
VOLT_CMP_OP( SInt, EqEq, IEq )
VOLT_CMP_OP( SInt, NotEq, INe )
// `===` is CaseLowering's synthesized `pattern === target`
// (rules/core-ast.md); on a primitive receiver it is structural equality, the
// same predicate as `==`.
VOLT_CMP_OP( SInt, TripleEq, IEq )
VOLT_CMP_OP( SInt, Lt, SLt )
VOLT_CMP_OP( SInt, Gt, SGt )
VOLT_CMP_OP( SInt, Le, SLe )
VOLT_CMP_OP( SInt, Ge, SGe )

// --- Comparisons, unsigned integers ----------------------------------------
VOLT_CMP_OP( UInt, EqEq, IEq )
VOLT_CMP_OP( UInt, NotEq, INe )
VOLT_CMP_OP( UInt, TripleEq, IEq )
VOLT_CMP_OP( UInt, Lt, ULt )
VOLT_CMP_OP( UInt, Gt, UGt )
VOLT_CMP_OP( UInt, Le, ULe )
VOLT_CMP_OP( UInt, Ge, UGe )

// --- Comparisons, floats ---------------------------------------------------
// Ordered: a NaN operand answers false, which is what `<` means in every
// language that does not go out of its way to say otherwise. `!=` is the one
// exception — `a != b` must be true when either side is NaN — so it is the
// *unordered* predicate.
VOLT_CMP_OP( Float, EqEq, FOEq )
VOLT_CMP_OP( Float, NotEq, FUNe )
VOLT_CMP_OP( Float, TripleEq, FOEq )
VOLT_CMP_OP( Float, Lt, FOLt )
VOLT_CMP_OP( Float, Gt, FOGt )
VOLT_CMP_OP( Float, Le, FOLe )
VOLT_CMP_OP( Float, Ge, FOGe )

// --- Unary -----------------------------------------------------------------
VOLT_UN_OP( SInt, PlusPlus, Inc )
VOLT_UN_OP( SInt, MinusMinus, Dec )
VOLT_UN_OP( SInt, Minus, Neg )
VOLT_UN_OP( SInt, Tilde, BitNot )
VOLT_UN_OP( SInt, Bang, LogicalNot )
VOLT_UN_OP( SInt, KwNot, LogicalNot )

VOLT_UN_OP( UInt, PlusPlus, Inc )
VOLT_UN_OP( UInt, MinusMinus, Dec )
VOLT_UN_OP( UInt, Minus, Neg )
VOLT_UN_OP( UInt, Tilde, BitNot )
VOLT_UN_OP( UInt, Bang, LogicalNot )
VOLT_UN_OP( UInt, KwNot, LogicalNot )

VOLT_UN_OP( Float, PlusPlus, FInc )
VOLT_UN_OP( Float, MinusMinus, FDec )
VOLT_UN_OP( Float, Minus, FNeg )

#ifdef VOLT_BIN_OP_DEFAULTED
    #undef VOLT_BIN_OP_DEFAULTED
#endif
#ifdef VOLT_CMP_OP_DEFAULTED
    #undef VOLT_CMP_OP_DEFAULTED
#endif
#ifdef VOLT_UN_OP_DEFAULTED
    #undef VOLT_UN_OP_DEFAULTED
#endif

#undef VOLT_BIN_OP
#undef VOLT_CMP_OP
#undef VOLT_UN_OP
