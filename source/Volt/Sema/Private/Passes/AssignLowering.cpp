// AssignLowering.cpp — order 24. Normalises compound assignment:
//
//     target op= value   ->   target = Binary( op, target', value )
//
// where `target'` is a fresh copy of the target node, so the read side and
// the write side are two distinct AST nodes with two distinct roles.
//
// Until now `Assign::Op` was carried all the way to the backends and read by
// nobody: the TypeChecker's Assign branch ignores it, so `s += 1` on a type
// with no `+` resolved to nothing at all. Desugaring here routes every
// compound form through the same operator resolution as `a + b`, and leaves
// exactly one shape of Assign — a store — in the core AST.
//
// The operator set is not hardcoded: `IsAssignment` comes from the
// VOLT_ASSIGN rows of Pratt.inl, and the base operator is derived from the
// spelling (`+=` minus its `=` is `+`). A new compound operator is one line
// in the manifest and nothing here.

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/Parser/Pratt.hpp"
#include "Volt/Sema/Pass.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

namespace
{

using namespace Volt;

// The infix operator behind a compound assignment, found by spelling rather
// than by a hand-kept table: `+=` is the punctuation spelled `+` plus `=`.
// Returns Error when Op is not a compound assignment.
[[nodiscard]] Frontend::TokenKind BaseOperatorOf ( Frontend::TokenKind Op )
{
    if ( Op == Frontend::TokenKind::Assign or not Frontend::IsAssignment( Op ) )
    {
        return Frontend::TokenKind::Error;
    }

    const std::string_view Spelling = Frontend::TokenSpelling( Op );
    if ( Spelling.size() < 2 or Spelling.back() != '=' )
    {
        return Frontend::TokenKind::Error;
    }

    const std::string_view Base = Spelling.substr( 0, Spelling.size() - 1 );
    for ( std::size_t Kind = 0; Kind < static_cast<std::size_t>( Frontend::TokenKind::Count ); ++Kind )
    {
        const auto Candidate = static_cast<Frontend::TokenKind>( Kind );
        if ( not Frontend::IsAssignment( Candidate ) and Frontend::TokenSpelling( Candidate ) == Base )
        {
            return Candidate;
        }
    }
    return Frontend::TokenKind::Error;
}

// A node that may be read twice without changing the meaning of the program.
// Deliberately shallow: a Call or an operator could run user code, and the
// compiler has no purity information to lean on.
[[nodiscard]] bool IsSideEffectFree ( const Frontend::ExprNode &Node )
{
    switch ( KindOf( Node ) )
    {
    case Frontend::ExprKind::Identifier:
    case Frontend::ExprKind::InstanceVar:
    case Frontend::ExprKind::SelfExpr:
    case Frontend::ExprKind::IntLiteral:
    case Frontend::ExprKind::FloatLiteral:
    case Frontend::ExprKind::StringLiteral:
    case Frontend::ExprKind::CharLiteral:
    case Frontend::ExprKind::BoolLiteral:
    case Frontend::ExprKind::SymbolLiteral:
    case Frontend::ExprKind::NilLiteral:
        return true;
    default:
        return false;
    }
}

class AssignRewriter
{

public:

    explicit AssignRewriter ( Sema::PassContext &InContext ) : Context( InContext ), Ast( InContext.Ast )
    {
    }

    std::size_t Run ()
    {
        // Index sweep, copy-out / write-back — see rules/ast-rewrite.md.
        const std::size_t OriginalCount = Ast.ExprCount();
        std::size_t Rewritten           = 0;

        for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            if ( KindOf( Ast.Expr( Id ) ) != Frontend::ExprKind::Assign )
            {
                continue;
            }

            const Frontend::Assign Node = std::get<Frontend::Assign>( Ast.Expr( Id ) );
            if ( BaseOperatorOf( Node.Op ) == Frontend::TokenKind::Error )
            {
                continue; // a plain `=`, already a store
            }

            if ( const std::optional<Frontend::ExprNode> Lowered = LowerCompound( Node ) )
            {
                Ast.Expr( Id ) = *Lowered;
                ++Rewritten;
            }
        }

        return Rewritten;
    }

private:

    // A node this pass mints is a *second occurrence of the same use*, so it
    // inherits the target's binding rather than being left unresolved:
    // ScopeResolver ran at order 10 and will not run again, and every consumer
    // — TypeChecker's local types, codegen's slot table — reaches a local
    // exclusively through BindingOf. Counting it is right too: `x += 1` reads
    // x. Nothing to inherit (a Member/Index target, or an unbound name) simply
    // leaves the clone as it was.
    [[nodiscard]] Frontend::ExprId CloneUse ( Frontend::ExprId TargetId, Frontend::ExprNode Node )
    {
        const Frontend::ExprId Fresh = Ast.Add( std::move( Node ) );
        if ( const Sema::Binding *Bound = Context.Scopes.BindingOf( TargetId ) )
        {
            Context.Scopes.BindUse( Fresh, *Bound );
        }
        return Fresh;
    }

    // A fresh node playing the *read* role, sharing the target's children.
    // Sharing is what makes `arr[i] += 1` legal without a temporary: the
    // children are re-read, never re-evaluated for effect, which is why they
    // must all be side-effect free.
    [[nodiscard]] std::optional<Frontend::ExprId> CloneForRead ( Frontend::ExprId TargetId )
    {
        const Frontend::ExprNode &Target = Ast.Expr( TargetId );

        if ( IsSideEffectFree( Target ) )
        {
            return CloneUse( TargetId, Frontend::ExprNode{ Target } );
        }

        if ( const auto *Mem = std::get_if<Frontend::Member>( &Target ) )
        {
            if ( not IsSideEffectFree( Ast.Expr( Mem->Object ) ) )
            {
                return std::nullopt;
            }
            return CloneUse( TargetId, Frontend::ExprNode{ *Mem } );
        }

        if ( const auto *Idx = std::get_if<Frontend::Index>( &Target ) )
        {
            if ( not IsSideEffectFree( Ast.Expr( Idx->Object ) ) )
            {
                return std::nullopt;
            }
            for ( const Frontend::ExprId Arg : Idx->Args )
            {
                if ( not IsSideEffectFree( Ast.Expr( Arg ) ) )
                {
                    return std::nullopt;
                }
            }
            return CloneUse( TargetId, Frontend::ExprNode{ *Idx } );
        }

        return std::nullopt;
    }

    [[nodiscard]] std::optional<Frontend::ExprNode> LowerCompound ( const Frontend::Assign &Node )
    {
        const std::optional<Frontend::ExprId> Read = CloneForRead( Node.Target );
        if ( not Read.has_value() )
        {
            Context.Diags.Report(
                Core::Diagnostic{ .Severity = Core::ESeverity::Error,
                                  .Range    = Node.Loc,
                                  .Message  = "'" + std::string{ Frontend::TokenSpelling( Node.Op ) } +
                                             "' needs a target that can be read twice — assign through a local instead",
                                  .Notes = {} } );
            return std::nullopt;
        }

        Frontend::Binary Combined;
        Combined.Loc = Node.Loc;
        Combined.Op  = BaseOperatorOf( Node.Op );
        Combined.Lhs = *Read;
        Combined.Rhs = Node.Value;

        Frontend::Assign Store;
        Store.Loc    = Node.Loc;
        Store.Op     = Frontend::TokenKind::Assign;
        Store.Target = Node.Target;
        Store.Value  = Ast.Add( Frontend::ExprNode{ Combined } );
        return Frontend::ExprNode{ Store };
    }

    Sema::PassContext &Context;
    Frontend::AstContext &Ast;
};

} // namespace

void Volt::Sema::AssignLowering ( Volt::Sema::PassContext &Context )
{
    Context.Stats.AssignsLowered += AssignRewriter( Context ).Run();
}
