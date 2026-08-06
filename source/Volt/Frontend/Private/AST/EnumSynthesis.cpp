#include "Volt/Frontend/AST/EnumSynthesis.hpp"

#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace
{

using namespace Volt;

// Best-effort decode of a raw `IntLiteral` lexeme. Non-numeric or
// unparseable text leaves Out untouched: this is syntactic and never
// diagnoses — that is TypeChecker's job.
[[nodiscard]] bool TryDecodeInt ( const Frontend::AstContext &Context, Frontend::ExprId Id, std::int64_t &Out )
{
    if ( not Id.IsValid() or Frontend::KindOf( Context.Expr( Id ) ) != Frontend::ExprKind::IntLiteral )
    {
        return false;
    }

    const std::string_view Text = Context.Text( std::get<Frontend::IntLiteral>( Context.Expr( Id ) ).Raw );
    std::int64_t Value          = 0;
    const auto [Ptr, Error]     = std::from_chars( Text.data(), Text.data() + Text.size(), Value );
    if ( Error != std::errc{} )
    {
        return false;
    }

    Out = Value;
    return true;
}

[[nodiscard]] Frontend::ExprId MakeIntLiteral ( Frontend::AstContext &Context, std::int64_t Value, Core::SourceRange Loc )
{
    Frontend::IntLiteral Node;
    Node.Loc = Loc;
    Node.Raw = Context.Strings().Intern( std::to_string( Value ) );
    return Context.Add( Node );
}

// `Enum::Body` only ever holds pre-existing `DeclId`s — this mutates the
// `EnumCase` slots in place and never adds or reorders entries, so the
// `Enum` decl itself never changes and needs no copy-out/write-back
// (rules/ast-rewrite.md); only the `IntLiteral` `Add()`s below happen, and
// those land in the *Expr* arena, never the Decl arena this reference lives
// in — per-category arenas, same section of that rule.
void MaterializeEnumCaseOrdinals ( Frontend::AstContext &Context, Frontend::DeclId EnumId )
{
    const Frontend::DeclList &Body = std::get<Frontend::Enum>( Context.Decl( EnumId ) ).Body;

    std::int64_t NextImplicit = 0;

    for ( const Frontend::DeclId CaseId : Body )
    {
        if ( not CaseId.IsValid() or Frontend::KindOf( Context.Decl( CaseId ) ) != Frontend::DeclKind::EnumCase )
        {
            continue;
        }

        Frontend::EnumCase Case = std::get<Frontend::EnumCase>( Context.Decl( CaseId ) );

        if ( Case.Value.IsValid() )
        {
            std::int64_t Explicit = 0;
            if ( TryDecodeInt( Context, Case.Value, Explicit ) )
            {
                NextImplicit = Explicit + 1;
            }
            // Non-literal explicit values are out of scope for this
            // syntactic pass and leave NextImplicit unchanged.
            continue;
        }

        Case.Value                = MakeIntLiteral( Context, NextImplicit, Case.Loc );
        Context.Decl( CaseId )    = Frontend::DeclNode{ std::move( Case ) };
        ++NextImplicit;
    }
}

[[nodiscard]] bool HasPayload ( const Frontend::AstContext &Context, const Frontend::DeclList &Body )
{
    return std::ranges::any_of( Body,
                                [&] ( const Frontend::DeclId Child )
                                {
                                    const auto *Case =
                                        Child.IsValid() ? std::get_if<Frontend::EnumCase>( &Context.Decl( Child ) ) : nullptr;
                                    return Case != nullptr and Case->Payload.Size() > 0;
                                } );
}

[[nodiscard]] bool HasMethodNamed ( const Frontend::AstContext &Context, const Frontend::DeclList &Body, std::string_view Name )
{
    return std::ranges::any_of( Body,
                                [&] ( const Frontend::DeclId Child )
                                {
                                    const auto *Existing =
                                        Child.IsValid() ? std::get_if<Frontend::Method>( &Context.Decl( Child ) ) : nullptr;
                                    return Existing != nullptr and Context.Text( Existing->Name ) == Name;
                                } );
}

} // namespace

void Volt::Frontend::MaterializeEnumOrdinals ( AstContext &Context )
{
    // Only decls present before this runs can be an Enum; the IntLiterals
    // synthesized above land in the Expr arena, so the Decl count never
    // grows here and no re-scan is needed.
    const std::size_t OriginalDeclCount = Context.DeclCount();

    for ( std::size_t Index = 0; Index < OriginalDeclCount; ++Index )
    {
        const DeclId Id{ static_cast<DeclId::ValueType>( Index ) };
        if ( KindOf( Context.Decl( Id ) ) == DeclKind::Enum )
        {
            MaterializeEnumCaseOrdinals( Context, Id );
        }
    }
}

void Volt::Frontend::SynthesizeEnumMembers ( AstContext &Context )
{
    // Copy-out-before-Add() (rules/ast-rewrite.md): `Type` is a value, not a
    // reference into the Decl arena, so the Expr/Stmt/Decl `Add()`s below
    // never invalidate anything this loop is holding. Bounded before the
    // first `Add()`: a synthesized `Method` lands in the Decl arena past
    // `OriginalDeclCount`, and it is never itself an `Enum`, so the loop
    // correctly never revisits it.
    const std::size_t OriginalDeclCount = Context.DeclCount();

    for ( std::size_t Index = 0; Index < OriginalDeclCount; ++Index )
    {
        const DeclId Id{ static_cast<DeclId::ValueType>( Index ) };
        if ( KindOf( Context.Decl( Id ) ) != DeclKind::Enum )
        {
            continue;
        }

        Enum Type = std::get<Enum>( Context.Decl( Id ) );

        if ( HasPayload( Context, Type.Body ) or HasMethodNamed( Context, Type.Body, "to_value" ) )
        {
            continue;
        }

        SelfExpr Self;
        Self.Loc             = Type.Loc;
        const ExprId SelfId  = Context.Add( Self );

        ExprStmt Tail;
        Tail.Loc  = Type.Loc;
        Tail.Expr = SelfId;

        Method Def;
        Def.Loc  = Type.Loc;
        Def.Name = Context.Strings().Intern( "to_value" );
        // ReturnType left invalid — see EnumSynthesis.hpp's comment.
        Def.Body.PushBack( Context.Add( Tail ) );
        Type.Body.PushBack( Context.Add( Def ) );

        Context.Decl( Id ) = DeclNode{ std::move( Type ) };
    }
}
