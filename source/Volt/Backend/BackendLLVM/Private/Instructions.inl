// Instructions.inl — the single manifest of primitive machine operations.
//
// `a + b` on a primitive is an instruction, not a method body: when
// UnitCallees left the operator *unresolved*, the receiver's
// Primitive{ Spelling, Bits } is what selects it (rules/core-ast.md,
// rules/zero-hardcode.md). The compiler reads one character of the spelling to
// place it in a family and nothing else — it never learns that "f64" is any
// particular Volt type.
//
// Three families, derived from the spelling and never from a name:
//
//   SInt   spelling starts with 'i' and is wider than one bit, or is "ptr"
//   UInt   spelling starts with 'u'
//   Float  spelling starts with 'f'
//
// The signed/unsigned split exists exactly where the machine has two
// instructions for one operator (`sdiv`/`udiv`, `icmp slt`/`icmp ult`) and
// nowhere else — `add` is the same instruction either way, so it is written
// once per family rather than made conditional.
//
// Re-included with different definitions of the three macros to generate the
// tables. Adding an operator is one line here; adding a *family* is one row of
// EOpFamily plus its lines.
//
//   VOLT_LLVM_BINOP( Family, Token, Opcode )     llvm::Instruction::BinaryOps
//   VOLT_LLVM_CMP( Family, Token, Predicate )    llvm::CmpInst::Predicate
//   VOLT_LLVM_UNOP( Family, Token, Kind )        EUnaryOp
//
// Deliberately *not* here, because none of them is a table lookup:
//   - `and` / `or` (and `&&` / `||`) short-circuit, so they are control flow;
//   - `not` / `!` on a one-bit value is `xor true`, which needs the operand's
//     width, so it is EUnaryOp::LogicalNot rather than a BinaryOps row;
//   - pointer `+` / `-` are heterogeneous (`( offset : UInt64 ) -> Pointer<T>`,
//     declared on the pointer nominal itself) and emit a `gep` whose stride is
//     the pointee's size — an address computation, not an arithmetic opcode.

#ifndef VOLT_LLVM_BINOP
    #define VOLT_LLVM_BINOP( Family, Token, Opcode )
#endif
#ifndef VOLT_LLVM_CMP
    #define VOLT_LLVM_CMP( Family, Token, Predicate )
#endif
#ifndef VOLT_LLVM_UNOP
    #define VOLT_LLVM_UNOP( Family, Token, Kind )
#endif

// --- Signed integers -------------------------------------------------------
VOLT_LLVM_BINOP( SInt, Plus, Add )
VOLT_LLVM_BINOP( SInt, Minus, Sub )
VOLT_LLVM_BINOP( SInt, Star, Mul )
VOLT_LLVM_BINOP( SInt, Slash, SDiv )
VOLT_LLVM_BINOP( SInt, Percent, SRem )
VOLT_LLVM_BINOP( SInt, Amp, And )
VOLT_LLVM_BINOP( SInt, Pipe, Or )
VOLT_LLVM_BINOP( SInt, Caret, Xor )
VOLT_LLVM_BINOP( SInt, Shl, Shl )
VOLT_LLVM_BINOP( SInt, Shr, AShr ) // arithmetic: the sign bit is meaningful

VOLT_LLVM_CMP( SInt, EqEq, ICMP_EQ )
VOLT_LLVM_CMP( SInt, NotEq, ICMP_NE )
// `===` is `CaseLowering`'s synthesized `pattern === target` (rules/core-ast.md);
// on a primitive receiver it is structural equality, same predicate as `==`.
VOLT_LLVM_CMP( SInt, TripleEq, ICMP_EQ )
VOLT_LLVM_CMP( SInt, Lt, ICMP_SLT )
VOLT_LLVM_CMP( SInt, Gt, ICMP_SGT )
VOLT_LLVM_CMP( SInt, Le, ICMP_SLE )
VOLT_LLVM_CMP( SInt, Ge, ICMP_SGE )

VOLT_LLVM_UNOP( SInt, Minus, Neg )
VOLT_LLVM_UNOP( SInt, Tilde, BitNot )
VOLT_LLVM_UNOP( SInt, Bang, LogicalNot )
VOLT_LLVM_UNOP( SInt, KwNot, LogicalNot )

// --- Unsigned integers -----------------------------------------------------
VOLT_LLVM_BINOP( UInt, Plus, Add )
VOLT_LLVM_BINOP( UInt, Minus, Sub )
VOLT_LLVM_BINOP( UInt, Star, Mul )
VOLT_LLVM_BINOP( UInt, Slash, UDiv )
VOLT_LLVM_BINOP( UInt, Percent, URem )
VOLT_LLVM_BINOP( UInt, Amp, And )
VOLT_LLVM_BINOP( UInt, Pipe, Or )
VOLT_LLVM_BINOP( UInt, Caret, Xor )
VOLT_LLVM_BINOP( UInt, Shl, Shl )
VOLT_LLVM_BINOP( UInt, Shr, LShr ) // logical: there is no sign bit to preserve

VOLT_LLVM_CMP( UInt, EqEq, ICMP_EQ )
VOLT_LLVM_CMP( UInt, NotEq, ICMP_NE )
VOLT_LLVM_CMP( UInt, TripleEq, ICMP_EQ )
VOLT_LLVM_CMP( UInt, Lt, ICMP_ULT )
VOLT_LLVM_CMP( UInt, Gt, ICMP_UGT )
VOLT_LLVM_CMP( UInt, Le, ICMP_ULE )
VOLT_LLVM_CMP( UInt, Ge, ICMP_UGE )

VOLT_LLVM_UNOP( UInt, Minus, Neg )
VOLT_LLVM_UNOP( UInt, Tilde, BitNot )
VOLT_LLVM_UNOP( UInt, Bang, LogicalNot )
VOLT_LLVM_UNOP( UInt, KwNot, LogicalNot )

// --- Floats ----------------------------------------------------------------
VOLT_LLVM_BINOP( Float, Plus, FAdd )
VOLT_LLVM_BINOP( Float, Minus, FSub )
VOLT_LLVM_BINOP( Float, Star, FMul )
VOLT_LLVM_BINOP( Float, Slash, FDiv )
VOLT_LLVM_BINOP( Float, Percent, FRem )

// Ordered comparisons: a NaN operand answers false, which is what `<` means in
// every language that does not go out of its way to say otherwise. `!=` is the
// one exception — `a != b` must be true when either side is NaN, so it is the
// *unordered* predicate.
VOLT_LLVM_CMP( Float, EqEq, FCMP_OEQ )
VOLT_LLVM_CMP( Float, NotEq, FCMP_UNE )
VOLT_LLVM_CMP( Float, TripleEq, FCMP_OEQ )
VOLT_LLVM_CMP( Float, Lt, FCMP_OLT )
VOLT_LLVM_CMP( Float, Gt, FCMP_OGT )
VOLT_LLVM_CMP( Float, Le, FCMP_OLE )
VOLT_LLVM_CMP( Float, Ge, FCMP_OGE )

VOLT_LLVM_UNOP( Float, Minus, FNeg )

#undef VOLT_LLVM_BINOP
#undef VOLT_LLVM_CMP
#undef VOLT_LLVM_UNOP
