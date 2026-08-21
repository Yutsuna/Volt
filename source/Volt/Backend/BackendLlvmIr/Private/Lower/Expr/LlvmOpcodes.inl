// LlvmOpcodes.inl — the LLVM column of BackendCore/Instructions.inl.
//
// One row per neutral opcode, giving its LLVM encoding. The *rows* — which
// family × operator selects which opcode — are stated once, upstream, in
// BackendCore/Instructions.inl; this file adds a column to that manifest and
// never repeats a row (rules/meta-first.md). A wasm emitter adds its own
// column beside this one.

#ifndef VOLT_LLVM_BIN
    #define VOLT_LLVM_BIN( Neutral, Encoding )
#endif

#ifndef VOLT_LLVM_CMP
    #define VOLT_LLVM_CMP( Neutral, Encoding )
#endif

VOLT_LLVM_BIN( Add, Add )
VOLT_LLVM_BIN( Sub, Sub )
VOLT_LLVM_BIN( Mul, Mul )
VOLT_LLVM_BIN( SDiv, SDiv )
VOLT_LLVM_BIN( UDiv, UDiv )
VOLT_LLVM_BIN( SRem, SRem )
VOLT_LLVM_BIN( URem, URem )
VOLT_LLVM_BIN( And, And )
VOLT_LLVM_BIN( Or, Or )
VOLT_LLVM_BIN( Xor, Xor )
VOLT_LLVM_BIN( Shl, Shl )
VOLT_LLVM_BIN( AShr, AShr )
VOLT_LLVM_BIN( LShr, LShr )
VOLT_LLVM_BIN( FAdd, FAdd )
VOLT_LLVM_BIN( FSub, FSub )
VOLT_LLVM_BIN( FMul, FMul )
VOLT_LLVM_BIN( FDiv, FDiv )
VOLT_LLVM_BIN( FRem, FRem )

VOLT_LLVM_CMP( IEq, ICMP_EQ )
VOLT_LLVM_CMP( INe, ICMP_NE )
VOLT_LLVM_CMP( SLt, ICMP_SLT )
VOLT_LLVM_CMP( SGt, ICMP_SGT )
VOLT_LLVM_CMP( SLe, ICMP_SLE )
VOLT_LLVM_CMP( SGe, ICMP_SGE )
VOLT_LLVM_CMP( ULt, ICMP_ULT )
VOLT_LLVM_CMP( UGt, ICMP_UGT )
VOLT_LLVM_CMP( ULe, ICMP_ULE )
VOLT_LLVM_CMP( UGe, ICMP_UGE )
VOLT_LLVM_CMP( FOEq, FCMP_OEQ )
VOLT_LLVM_CMP( FUNe, FCMP_UNE )
VOLT_LLVM_CMP( FOLt, FCMP_OLT )
VOLT_LLVM_CMP( FOGt, FCMP_OGT )
VOLT_LLVM_CMP( FOLe, FCMP_OLE )
VOLT_LLVM_CMP( FOGe, FCMP_OGE )

#undef VOLT_LLVM_BIN
#undef VOLT_LLVM_CMP
