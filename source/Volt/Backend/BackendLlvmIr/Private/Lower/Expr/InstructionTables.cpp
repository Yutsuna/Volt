// InstructionTables.cpp — see InstructionTables.hpp.
//
// Both mappings are total switches with no `default`: a neutral opcode added to
// BackendCore without an LLVM encoding is then a `-Wswitch` error here rather
// than a silent fallthrough at run time.

#include "Lower/Expr/InstructionTables.hpp"

llvm::Instruction::BinaryOps Volt::Backend::Llvm::EncodingOf ( EBinOp Op )
{
    switch ( Op )
    {
#define VOLT_LLVM_BIN( Neutral, Encoding )                                                                                       \
    case EBinOp::Neutral:                                                                                                        \
        return llvm::Instruction::BinaryOps::Encoding;
#include "Lower/Expr/LlvmOpcodes.inl"
    }
    return llvm::Instruction::BinaryOps::Add;
}

llvm::CmpInst::Predicate Volt::Backend::Llvm::EncodingOf ( ECmpPred Predicate )
{
    switch ( Predicate )
    {
#define VOLT_LLVM_CMP( Neutral, Encoding )                                                                                       \
    case ECmpPred::Neutral:                                                                                                      \
        return llvm::CmpInst::Predicate::Encoding;
#include "Lower/Expr/LlvmOpcodes.inl"
    }
    return llvm::CmpInst::Predicate::ICMP_EQ;
}
