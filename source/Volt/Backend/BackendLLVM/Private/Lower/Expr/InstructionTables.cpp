// InstructionTables.cpp — the three tables themselves. See the header for why
// they are written out rather than macro-generated.

#include "Lower/Expr/InstructionTables.hpp"

#include <span>

namespace
{

using Volt::Backend::Llvm::BinOpRow;
using Volt::Backend::Llvm::CmpRow;
using Volt::Backend::Llvm::EOpFamily;
using Volt::Backend::Llvm::EUnaryOp;
using Volt::Backend::Llvm::UnOpRow;
using Volt::Frontend::TokenKind;

constexpr BinOpRow BinOps[] = {
    // --- Signed integers ---------------------------------------------------
    { EOpFamily::SInt, TokenKind::Plus, llvm::Instruction::BinaryOps::Add },
    { EOpFamily::SInt, TokenKind::Minus, llvm::Instruction::BinaryOps::Sub },
    { EOpFamily::SInt, TokenKind::Star, llvm::Instruction::BinaryOps::Mul },
    { EOpFamily::SInt, TokenKind::Slash, llvm::Instruction::BinaryOps::SDiv },
    { EOpFamily::SInt, TokenKind::Percent, llvm::Instruction::BinaryOps::SRem },
    { EOpFamily::SInt, TokenKind::Amp, llvm::Instruction::BinaryOps::And },
    { EOpFamily::SInt, TokenKind::Pipe, llvm::Instruction::BinaryOps::Or },
    { EOpFamily::SInt, TokenKind::Caret, llvm::Instruction::BinaryOps::Xor },
    { EOpFamily::SInt, TokenKind::Shl, llvm::Instruction::BinaryOps::Shl },
    { EOpFamily::SInt, TokenKind::Shr, llvm::Instruction::BinaryOps::AShr }, // arithmetic: the sign bit is meaningful

    // --- Unsigned integers -------------------------------------------------
    { EOpFamily::UInt, TokenKind::Plus, llvm::Instruction::BinaryOps::Add },
    { EOpFamily::UInt, TokenKind::Minus, llvm::Instruction::BinaryOps::Sub },
    { EOpFamily::UInt, TokenKind::Star, llvm::Instruction::BinaryOps::Mul },
    { EOpFamily::UInt, TokenKind::Slash, llvm::Instruction::BinaryOps::UDiv },
    { EOpFamily::UInt, TokenKind::Percent, llvm::Instruction::BinaryOps::URem },
    { EOpFamily::UInt, TokenKind::Amp, llvm::Instruction::BinaryOps::And },
    { EOpFamily::UInt, TokenKind::Pipe, llvm::Instruction::BinaryOps::Or },
    { EOpFamily::UInt, TokenKind::Caret, llvm::Instruction::BinaryOps::Xor },
    { EOpFamily::UInt, TokenKind::Shl, llvm::Instruction::BinaryOps::Shl },
    { EOpFamily::UInt, TokenKind::Shr, llvm::Instruction::BinaryOps::LShr }, // logical: there is no sign bit to preserve

    // --- Floats ------------------------------------------------------------
    { EOpFamily::Float, TokenKind::Plus, llvm::Instruction::BinaryOps::FAdd },
    { EOpFamily::Float, TokenKind::Minus, llvm::Instruction::BinaryOps::FSub },
    { EOpFamily::Float, TokenKind::Star, llvm::Instruction::BinaryOps::FMul },
    { EOpFamily::Float, TokenKind::Slash, llvm::Instruction::BinaryOps::FDiv },
    { EOpFamily::Float, TokenKind::Percent, llvm::Instruction::BinaryOps::FRem },
};

constexpr CmpRow Cmps[] = {
    // --- Signed integers ---------------------------------------------------
    { EOpFamily::SInt, TokenKind::EqEq, llvm::CmpInst::ICMP_EQ },
    { EOpFamily::SInt, TokenKind::NotEq, llvm::CmpInst::ICMP_NE },
    // `===` is `CaseLowering`'s synthesized `pattern === target`
    // (rules/core-ast.md); on a primitive receiver it is structural equality,
    // same predicate as `==`.
    { EOpFamily::SInt, TokenKind::TripleEq, llvm::CmpInst::ICMP_EQ },
    { EOpFamily::SInt, TokenKind::Lt, llvm::CmpInst::ICMP_SLT },
    { EOpFamily::SInt, TokenKind::Gt, llvm::CmpInst::ICMP_SGT },
    { EOpFamily::SInt, TokenKind::Le, llvm::CmpInst::ICMP_SLE },
    { EOpFamily::SInt, TokenKind::Ge, llvm::CmpInst::ICMP_SGE },

    // --- Unsigned integers -------------------------------------------------
    { EOpFamily::UInt, TokenKind::EqEq, llvm::CmpInst::ICMP_EQ },
    { EOpFamily::UInt, TokenKind::NotEq, llvm::CmpInst::ICMP_NE },
    { EOpFamily::UInt, TokenKind::TripleEq, llvm::CmpInst::ICMP_EQ },
    { EOpFamily::UInt, TokenKind::Lt, llvm::CmpInst::ICMP_ULT },
    { EOpFamily::UInt, TokenKind::Gt, llvm::CmpInst::ICMP_UGT },
    { EOpFamily::UInt, TokenKind::Le, llvm::CmpInst::ICMP_ULE },
    { EOpFamily::UInt, TokenKind::Ge, llvm::CmpInst::ICMP_UGE },

    // --- Floats ------------------------------------------------------------
    // Ordered comparisons: a NaN operand answers false, which is what `<` means
    // in every language that does not go out of its way to say otherwise. `!=`
    // is the one exception — `a != b` must be true when either side is NaN, so
    // it is the *unordered* predicate.
    { EOpFamily::Float, TokenKind::EqEq, llvm::CmpInst::FCMP_OEQ },
    { EOpFamily::Float, TokenKind::NotEq, llvm::CmpInst::FCMP_UNE },
    { EOpFamily::Float, TokenKind::TripleEq, llvm::CmpInst::FCMP_OEQ },
    { EOpFamily::Float, TokenKind::Lt, llvm::CmpInst::FCMP_OLT },
    { EOpFamily::Float, TokenKind::Gt, llvm::CmpInst::FCMP_OGT },
    { EOpFamily::Float, TokenKind::Le, llvm::CmpInst::FCMP_OLE },
    { EOpFamily::Float, TokenKind::Ge, llvm::CmpInst::FCMP_OGE },
};

constexpr UnOpRow UnOps[] = {
    // --- Signed integers ---------------------------------------------------
    { EOpFamily::SInt, TokenKind::PlusPlus, EUnaryOp::Inc },
    { EOpFamily::SInt, TokenKind::MinusMinus, EUnaryOp::Dec },
    { EOpFamily::SInt, TokenKind::Minus, EUnaryOp::Neg },
    { EOpFamily::SInt, TokenKind::Tilde, EUnaryOp::BitNot },
    { EOpFamily::SInt, TokenKind::Bang, EUnaryOp::LogicalNot },
    { EOpFamily::SInt, TokenKind::KwNot, EUnaryOp::LogicalNot },

    // --- Unsigned integers -------------------------------------------------
    { EOpFamily::UInt, TokenKind::PlusPlus, EUnaryOp::Inc },
    { EOpFamily::UInt, TokenKind::MinusMinus, EUnaryOp::Dec },
    { EOpFamily::UInt, TokenKind::Minus, EUnaryOp::Neg },
    { EOpFamily::UInt, TokenKind::Tilde, EUnaryOp::BitNot },
    { EOpFamily::UInt, TokenKind::Bang, EUnaryOp::LogicalNot },
    { EOpFamily::UInt, TokenKind::KwNot, EUnaryOp::LogicalNot },

    // --- Floats ------------------------------------------------------------
    { EOpFamily::Float, TokenKind::PlusPlus, EUnaryOp::FInc },
    { EOpFamily::Float, TokenKind::MinusMinus, EUnaryOp::FDec },
    { EOpFamily::Float, TokenKind::Minus, EUnaryOp::FNeg },
};

template <typename Row> [[nodiscard]] const Row *FindRow ( std::span<const Row> Rows, EOpFamily Family, TokenKind Op )
{
    for ( const Row &Entry : Rows )
    {
        if ( Entry.Family == Family and Entry.Op == Op )
        {
            return &Entry;
        }
    }
    return nullptr;
}

} // namespace

Volt::Backend::Llvm::EOpFamily Volt::Backend::Llvm::FamilyOf ( std::string_view Spelling )
{
    if ( Spelling.empty() )
    {
        return EOpFamily::None;
    }
    if ( Spelling == "ptr" )
    {
        return EOpFamily::UInt;
    }

    switch ( Spelling.front() )
    {
    case 'i':
        return EOpFamily::SInt;
    case 'u':
        return EOpFamily::UInt;
    case 'f':
        return EOpFamily::Float;
    default:
        return EOpFamily::None;
    }
}

const Volt::Backend::Llvm::BinOpRow *Volt::Backend::Llvm::FindBinOp ( EOpFamily Family, Frontend::TokenKind Op )
{
    return FindRow( std::span<const BinOpRow>{ BinOps }, Family, Op );
}

const Volt::Backend::Llvm::CmpRow *Volt::Backend::Llvm::FindCmp ( EOpFamily Family, Frontend::TokenKind Op )
{
    return FindRow( std::span<const CmpRow>{ Cmps }, Family, Op );
}

const Volt::Backend::Llvm::UnOpRow *Volt::Backend::Llvm::FindUnOp ( EOpFamily Family, Frontend::TokenKind Op )
{
    return FindRow( std::span<const UnOpRow>{ UnOps }, Family, Op );
}
