#pragma once

// InstructionSchema.hpp — the vocabulary Instructions.inl is written in.
//
// The manifest next door names a family, a token and a *target-neutral*
// opcode; this header declares those three enums and the three lookups that
// read the rows. Nothing here mentions a target: LLVM maps EBinOp onto
// llvm::Instruction::BinaryOps in its own file, a wasm emitter would map the
// same enum onto its own byte, and neither re-lists a row.
//
// The vocabulary is exactly the machine one rules/backend-machine-only.md
// grants: widths, signedness, and the operations a CPU has. No Volt type name
// reaches it — the family comes from one character of an opaque spelling.

#include "BackendCore_export.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <cstdint>
#include <string_view>

namespace Volt
{

namespace Backend
{

    // The family a spelling belongs to, derived from one character:
    //
    //   SInt   starts with 'i'
    //   UInt   starts with 'u', and "ptr" — an address has no sign bit, so
    //          `p < q` is an unsigned compare
    //   Float  starts with 'f'
    enum class EOpFamily : std::uint8_t
    {

        None,
        SInt,
        UInt,
        Float,
    };

    // Binary machine operations. The signed/unsigned pairs exist only where the
    // machine really has two instructions for one operator.
    enum class EBinOp : std::uint8_t
    {

        Add,
        Sub,
        Mul,
        SDiv,
        UDiv,
        SRem,
        URem,
        And,
        Or,
        Xor,
        Shl,
        AShr,
        LShr,
        FAdd,
        FSub,
        FMul,
        FDiv,
        FRem,
    };

    // Comparison predicates. `FO*` is ordered (a NaN operand answers false);
    // `FUNe` is the one unordered form, because `a != b` must be true when
    // either side is NaN.
    enum class ECmpPred : std::uint8_t
    {

        IEq,
        INe,
        SLt,
        SGt,
        SLe,
        SGe,
        ULt,
        UGt,
        ULe,
        UGe,
        FOEq,
        FUNe,
        FOLt,
        FOGt,
        FOLe,
        FOGe,
    };

    // The unary forms, which are not binary rows: each needs the operand's own
    // width or type to build its constant.
    enum class EUnaryOp : std::uint8_t
    {

        Neg,        // 0 - x
        FNeg,       // fneg x
        BitNot,     // x xor -1
        LogicalNot, // x xor true — the one-bit case
        Inc,        // x + 1
        FInc,       // x + 1.0
        Dec,        // x - 1
        FDec,       // x - 1.0
    };

    struct BinOpRow
    {

        EOpFamily Family = EOpFamily::None;
        Frontend::TokenKind Op{};
        EBinOp Opcode{};
    };

    struct CmpRow
    {

        EOpFamily Family = EOpFamily::None;
        Frontend::TokenKind Op{};
        ECmpPred Predicate{};
    };

    struct UnOpRow
    {

        EOpFamily Family = EOpFamily::None;
        Frontend::TokenKind Op{};
        EUnaryOp Kind{};
    };

    // The family of an opaque spelling. Empty for an aggregate, which has no
    // machine operation at all.
    [[nodiscard]] BACKENDCORE_EXPORT EOpFamily FamilyOf ( std::string_view Spelling );

    // Null when the family carries no such operator, which is what the caller
    // reports — never a guess.
    [[nodiscard]] BACKENDCORE_EXPORT const BinOpRow *FindBinOp ( EOpFamily Family, Frontend::TokenKind Op );
    [[nodiscard]] BACKENDCORE_EXPORT const CmpRow *FindCmp ( EOpFamily Family, Frontend::TokenKind Op );
    [[nodiscard]] BACKENDCORE_EXPORT const UnOpRow *FindUnOp ( EOpFamily Family, Frontend::TokenKind Op );

} // namespace Backend

} // namespace Volt
