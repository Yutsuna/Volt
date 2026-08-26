// InstructionSchema.cpp — the three tables, generated from the manifest.
//
// Instructions.inl is included three times, once per macro definition. The rows
// are written once, in one place, and no consumer ever re-lists them
// (rules/meta-first.md).

#include "Volt/BackendCore/InstructionSchema.hpp"

#include <span>

namespace
{

using Volt::Backend::BinOpRow;
using Volt::Backend::CmpRow;
using Volt::Backend::EBinOp;
using Volt::Backend::ECmpPred;
using Volt::Backend::EOpFamily;
using Volt::Backend::EUnaryOp;
using Volt::Backend::UnOpRow;
using Volt::Frontend::TokenKind;

#define VOLT_BIN_OP( Family, Token, Opcode ) { EOpFamily::Family, TokenKind::Token, EBinOp::Opcode },
constexpr BinOpRow BinOps[] = {
#include "Volt/BackendCore/Instructions.inl"
};

#define VOLT_CMP_OP( Family, Token, Predicate ) { EOpFamily::Family, TokenKind::Token, ECmpPred::Predicate },
constexpr CmpRow Cmps[] = {
#include "Volt/BackendCore/Instructions.inl"
};

#define VOLT_UN_OP( Family, Token, Kind ) { EOpFamily::Family, TokenKind::Token, EUnaryOp::Kind },
constexpr UnOpRow UnOps[] = {
#include "Volt/BackendCore/Instructions.inl"
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

Volt::Backend::EOpFamily Volt::Backend::FamilyOf ( std::string_view Spelling )
{
    if ( Spelling.empty() )
    {
        return EOpFamily::None;
    }
    // A Pointer layout is an address just as `@[Primitive("ptr")]` is, and the
    // two must select the same instructions.
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

const Volt::Backend::BinOpRow *Volt::Backend::FindBinOp ( EOpFamily Family, Frontend::TokenKind Op )
{
    return FindRow( std::span<const BinOpRow>{ BinOps }, Family, Op );
}

const Volt::Backend::CmpRow *Volt::Backend::FindCmp ( EOpFamily Family, Frontend::TokenKind Op )
{
    return FindRow( std::span<const CmpRow>{ Cmps }, Family, Op );
}

const Volt::Backend::UnOpRow *Volt::Backend::FindUnOp ( EOpFamily Family, Frontend::TokenKind Op )
{
    return FindRow( std::span<const UnOpRow>{ UnOps }, Family, Op );
}
