// TraitEngine.cpp — the five receiver traits, one manifest row each.
//
// Every evaluator below is a question about the TypeStore's own structure, and
// each is a single expression over the three traversals at the top of this
// file. That is the whole point of the shape: a sixth trait is one row in
// TraitOps.inl, one row in TokenKind.inl and one line here — never a branch
// added to the interception seam, and never anything in a backend.

#include "Volt/MiddleEnd/ConstEval/TraitEngine.hpp"

#include "Volt/MiddleEnd/TypeSystem/TypeResolve.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace
{

using namespace Volt;
using namespace Volt::MiddleEnd;
using namespace Volt::MiddleEnd::ConstEval;
using TypeSystem::EMemberKind;
using TypeSystem::NominalId;
using TypeSystem::NominalType;
using TypeSystem::SigTypeId;
using TypeSystem::TypeStore;

// A hierarchy this deep is malformed, and a cyclic one would otherwise hang
// sema. The same bound, for the same reason, as TypeStore::LookupMember's own.
constexpr std::uint32_t MaxDepth = 16;

// --- The three traversals every trait is written in terms of ---------------

// Does `Id` include `Mixin` — directly, through a mixin of a mixin, or through
// anything it inherits from? `include` is transitive in both directions: a
// class that inherits from an includer includes it too, which is exactly what
// LookupMember already assumes when it resolves a name through both chains.
[[nodiscard]] bool IncludesMixin ( const TypeStore &Types, NominalId Id, NominalId Mixin, std::uint32_t Depth = 0 )
{
    if ( not Id.IsValid() or not Mixin.IsValid() or Depth > MaxDepth )
    {
        return false;
    }

    const NominalType &Type = Types.Type( Id );
    for ( const SigTypeId Included : Type.Includes )
    {
        const NominalId Base = Types.BaseOf( Included );
        if ( Base == Mixin or IncludesMixin( Types, Base, Mixin, Depth + 1 ) )
        {
            return true;
        }
    }
    return IncludesMixin( Types, Types.BaseOf( Type.Super ), Mixin, Depth + 1 );
}

// Strictly above: a type does not inherit from itself. `IsSubclassOf` walks
// the same chain but answers reflexively, which is what `is_a?` wants and
// `inherits_from?` does not — the difference is this one comparison, so both
// read the same traversal rather than each keeping its own.
[[nodiscard]] bool InheritsFrom ( const TypeStore &Types, NominalId Id, NominalId Ancestor )
{
    return Id != Ancestor and TypeSystem::IsSubclassOf( Types, Id, Ancestor );
}

// Name existence *at a kind*. LookupMember already walks own body → mixins →
// superclass transitively; all this adds is the kind test, which is what
// separates `has_field?` from `has_method?` on the same name.
[[nodiscard]] bool HasMemberOfKind ( const TypeStore &Types, NominalId Id, std::string_view Name, EMemberKind Kind )
{
    const TypeStore::MemberRef Found = Types.LookupMember( Id, Name );
    return Found.Decl != nullptr and Found.Decl->Kind == Kind;
}

// --- The manifest ----------------------------------------------------------

#define VOLT_TRAIT( Name, Token, Operand ) [[nodiscard]] bool Trait##Name( const TraitSite &Site );
#include "Volt/MiddleEnd/ConstEval/TraitOps.inl"

using TraitFn = bool ( * )( const TraitSite &Site );

struct TraitRow
{

    Frontend::TokenKind Token = Frontend::TokenKind::Eof;
    EOperandKind Operand      = EOperandKind::Type;
    TraitFn Apply             = nullptr;
};

constexpr std::array TraitTable = {
#define VOLT_TRAIT( Name, Token, Operand ) TraitRow{ Frontend::TokenKind::Token, EOperandKind::Operand, &Trait##Name },
#include "Volt/MiddleEnd/ConstEval/TraitOps.inl"
};

// TokenKind.inl owns the spellings, TraitOps.inl owns the meanings, and this
// is what stops one from growing without the other: a VOLT_TRAIT_KEYWORD row
// with no VOLT_TRAIT row would parse and then resolve as an ordinary member,
// which is a silently wrong program rather than a build error.
constexpr std::size_t ReceiverTraitTokenCount ()
{
    std::size_t Count = 0;
#define VOLT_TRAIT_KEYWORD( Name, Spelling ) ++Count;
#include "Volt/Frontend/Lexer/TokenKind.inl"
    return Count;
}

static_assert( TraitTable.size() == ReceiverTraitTokenCount(),
               "every VOLT_TRAIT_KEYWORD row of TokenKind.inl needs exactly one VOLT_TRAIT row in TraitOps.inl" );

// --- The traits themselves -------------------------------------------------

bool TraitIncludes ( const TraitSite &Site )
{
    return IncludesMixin( Site.Types, Site.Receiver, Site.Operand );
}

bool TraitInheritsFrom ( const TraitSite &Site )
{
    return InheritsFrom( Site.Types, Site.Receiver, Site.Operand );
}

// The union of the other two plus identity — `x.is_a? T` is true when T is
// what x is, what x descends from, or a mixin x includes. A mixin *is* a type
// here, so the third arm is not a special case: it is the same "does this
// name sit above me" question the other two ask of the other chain.
bool TraitIsA ( const TraitSite &Site )
{
    return Site.Receiver == Site.Operand or TypeSystem::IsSubclassOf( Site.Types, Site.Receiver, Site.Operand ) or
           IncludesMixin( Site.Types, Site.Receiver, Site.Operand );
}

bool TraitHasField ( const TraitSite &Site )
{
    return HasMemberOfKind( Site.Types, Site.Receiver, Site.Name, EMemberKind::Field );
}

bool TraitHasMethod ( const TraitSite &Site )
{
    return HasMemberOfKind( Site.Types, Site.Receiver, Site.Name, EMemberKind::Method );
}

[[nodiscard]] const TraitRow *RowFor ( Frontend::TokenKind Trait )
{
    for ( const TraitRow &Row : TraitTable )
    {
        if ( Row.Token == Trait )
        {
            return &Row;
        }
    }
    return nullptr;
}

} // namespace

std::optional<Volt::Frontend::TokenKind> Volt::MiddleEnd::ConstEval::LookupTrait ( std::string_view Spelling )
{
    for ( const TraitRow &Row : TraitTable )
    {
        if ( Frontend::TokenSpelling( Row.Token ) == Spelling )
        {
            return Row.Token;
        }
    }
    return std::nullopt;
}

Volt::MiddleEnd::ConstEval::EOperandKind Volt::MiddleEnd::ConstEval::OperandOf ( Frontend::TokenKind Trait )
{
    const TraitRow *Row = RowFor( Trait );
    return Row != nullptr ? Row->Operand : EOperandKind::Type;
}

bool Volt::MiddleEnd::ConstEval::EvaluateTrait ( Frontend::TokenKind Trait, const TraitSite &Site )
{
    const TraitRow *Row = RowFor( Trait );
    return Row != nullptr and Site.Receiver.IsValid() and Row->Apply( Site );
}

std::span<const std::string_view> Volt::MiddleEnd::ConstEval::TraitNames ()
{
    static const std::array Names = {
#define VOLT_TRAIT_KEYWORD( Name, Spelling ) std::string_view{ Spelling },
#include "Volt/Frontend/Lexer/TokenKind.inl"
    };
    return Names;
}
