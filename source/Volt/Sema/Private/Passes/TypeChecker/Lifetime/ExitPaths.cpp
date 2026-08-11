#include "ExitPaths.hpp"

#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"

#include <cstdint>
#include <type_traits>
#include <unordered_set>
#include <variant>

namespace Volt::Sema::TypeCheckerPass::Lifetime
{

namespace
{

    // Forward declarations — Stmt and Expr recurse into each other.
    [[nodiscard]] bool ContainsExitStmt ( const Frontend::AstContext &Ast, Frontend::StmtId Id );
    [[nodiscard]] bool ContainsExitExpr ( const Frontend::AstContext &Ast, Frontend::ExprId Id );

    // A read-only structural descent (never a write, so no arena-rewrite hazard —
    // rules/ast-rewrite.md's checklist is about mutation, not this kind of scan)
    // over every Expr/Stmt reachable from Id, looking for a `Return`/`Break`/
    // `Next` anywhere. Used only as the Phase 4 safety net (see
    // ContainsUnstructuredExit below) — the structural recursion in ProcessBlock
    // handles every statement-position If/While/CaseExpr/BeginExpr itself; this
    // scan exists to catch the shapes it does not (an exit hiding inside an
    // expression-position control construct, e.g. `x = if c then return 1 else 2
    // end`) and bail rather than silently miss a finalize.
    template <typename NodeVariant> bool ScanExitFields ( const Frontend::AstContext &Ast, const NodeVariant &Variant )
    {
        bool bFound = false;
        std::visit(
            [&] ( const auto &Node )
            {
                using T = std::remove_cvref_t<decltype( Node )>;
                if constexpr ( not std::is_same_v<T, std::monostate> )
                {
                    Meta::ForEachField( Node,
                                        [&] ( const char *, const auto &Field )
                                        {
                                            if ( bFound )
                                            {
                                                return;
                                            }
                                            using F = std::remove_cvref_t<decltype( Field )>;
                                            if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                            {
                                                bFound = bFound or ContainsExitExpr( Ast, Field );
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                            {
                                                for ( const Frontend::ExprId Child : Field )
                                                {
                                                    bFound = bFound or ContainsExitExpr( Ast, Child );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                            {
                                                bFound = bFound or ContainsExitStmt( Ast, Field );
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                            {
                                                for ( const Frontend::StmtId Child : Field )
                                                {
                                                    bFound = bFound or ContainsExitStmt( Ast, Child );
                                                }
                                            }
                                        } );
                }
            },
            Variant );
        return bFound;
    }

    bool ContainsExitStmt ( const Frontend::AstContext &Ast, Frontend::StmtId Id )
    {
        if ( not Id.IsValid() )
        {
            return false;
        }
        const Frontend::StmtNode &Node = Ast.Stmt( Id );
        if ( std::holds_alternative<Frontend::Return>( Node ) or std::holds_alternative<Frontend::Break>( Node ) or
             std::holds_alternative<Frontend::Next>( Node ) )
        {
            return true;
        }
        return ScanExitFields( Ast, Node );
    }

    bool ContainsExitExpr ( const Frontend::AstContext &Ast, Frontend::ExprId Id )
    {
        if ( not Id.IsValid() )
        {
            return false;
        }
        return ScanExitFields( Ast, Ast.Expr( Id ) );
    }

} // namespace

// Phase 4 safety net: ProcessBlock recurses structurally into every
// statement-position If/While/CaseExpr/BeginExpr (the four StmtList-bearing
// constructs — Return/Break/Next can *only* ever live inside a StmtList, so
// this set is structurally exhaustive for anything reached this way). What
// it does not reach is a Return/Break/Next hiding inside an
// expression-position occurrence of one of those four (`x = if c then
// return 1 else 2 end`, a Call argument, ...) — rare in practice, and
// refused the same way Phase 1/3 refused whole classes of methods: leave the
// method completely untouched rather than risk missing a finalize.
[[nodiscard]] bool ContainsUnstructuredExit ( const Frontend::AstContext &Ast, const Frontend::StmtList &Body )
{
    for ( const Frontend::StmtId Id : Body )
    {
        if ( not Id.IsValid() )
        {
            continue;
        }
        const Frontend::StmtNode &Node = Ast.Stmt( Id );

        // A top-level Return/Break/Next is exactly what ProcessBlock's
        // splice step handles directly — not "unstructured".
        if ( std::holds_alternative<Frontend::Return>( Node ) or std::holds_alternative<Frontend::Break>( Node ) or
             std::holds_alternative<Frontend::Next>( Node ) )
        {
            continue;
        }

        if ( const auto *WhileNode = std::get_if<Frontend::While>( &Node ) )
        {
            if ( ContainsUnstructuredExit( Ast, WhileNode->Body ) )
            {
                return true;
            }
            continue;
        }

        if ( const auto *ExprStmtNode = std::get_if<Frontend::ExprStmt>( &Node ) )
        {
            const Frontend::ExprNode &Inner = Ast.Expr( ExprStmtNode->Expr );
            if ( const auto *IfNode = std::get_if<Frontend::If>( &Inner ) )
            {
                if ( ContainsUnstructuredExit( Ast, IfNode->Then ) or ContainsUnstructuredExit( Ast, IfNode->Else ) )
                {
                    return true;
                }
                continue;
            }
            if ( const auto *CaseNode = std::get_if<Frontend::CaseExpr>( &Inner ) )
            {
                for ( const Frontend::StmtId ClauseId : CaseNode->Clauses )
                {
                    const auto &Clause = std::get<Frontend::WhenClause>( Ast.Stmt( ClauseId ) );
                    if ( ContainsUnstructuredExit( Ast, Clause.Body ) )
                    {
                        return true;
                    }
                }
                if ( ContainsUnstructuredExit( Ast, CaseNode->ElseBody ) )
                {
                    return true;
                }
                continue;
            }
            if ( const auto *BeginNode = std::get_if<Frontend::BeginExpr>( &Inner ) )
            {
                if ( ContainsUnstructuredExit( Ast, BeginNode->Body ) )
                {
                    return true;
                }
                for ( const Frontend::StmtId RescueId : BeginNode->RescueClauses )
                {
                    const auto &Rescue = std::get<Frontend::RescueClause>( Ast.Stmt( RescueId ) );
                    if ( ContainsUnstructuredExit( Ast, Rescue.Body ) )
                    {
                        return true;
                    }
                }
                if ( ContainsUnstructuredExit( Ast, BeginNode->EnsureBody ) )
                {
                    return true;
                }
                continue;
            }
            // Anything else (Assign, a bare Call, ...) — fall back to the
            // exhaustive scan: it may still hide an exit inside an
            // expression-position If/CaseExpr/BeginExpr.
            if ( ContainsExitExpr( Ast, ExprStmtNode->Expr ) )
            {
                return true;
            }
            continue;
        }

        if ( ContainsExitStmt( Ast, Id ) )
        {
            return true;
        }
    }
    return false;
}

// Forward declarations — a StmtList's own tail and an expression's possible
// tail values recurse into each other (If/Ternary/CaseExpr/BeginExpr).
void CollectTailIdentifierNames ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::unordered_set<std::uint32_t> &Out );

// The move-out exemption (design decision 4a/4b) must be conservative about
// *every* path a value can reach the caller through, not just the literal
// syntactic tail — found the hard way: `result = "1" + "2"; flag ? "-" +
// result : result` only checks whether Body's OWN last statement is a bare
// Identifier, and here it's an `If`/`Ternary`, so `result` was never
// recognised as moved out — the method's own EnsureBody then freed the very
// buffer one branch had just handed back by reference, a use-after-free the
// caller's own next read (or double free, if that branch's value is later
// finalized again) can't recover from. This walks every branch of an
// If/Ternary/CaseExpr/BeginExpr tail and collects every name that could be
// the literal value handed back on *some* path — the caller of this
// function excludes all of them, never just the first found.
void CollectTailIdentifierNamesFromBody ( const Frontend::AstContext &Ast,
                                          const Frontend::StmtList &Body,
                                          std::unordered_set<std::uint32_t> &Out )
{
    if ( Body.IsEmpty() )
    {
        return;
    }
    if ( const auto *TailStmt = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Body[Body.Size() - 1] ) ) )
    {
        CollectTailIdentifierNames( Ast, TailStmt->Expr, Out );
    }
}

void CollectTailIdentifierNames ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::unordered_set<std::uint32_t> &Out )
{
    if ( not Id.IsValid() )
    {
        return;
    }
    if ( const auto *Ident = std::get_if<Frontend::Identifier>( &Ast.Expr( Id ) ) )
    {
        Out.insert( Ident->Name.Value );
        return;
    }
    if ( const auto *Tern = std::get_if<Frontend::Ternary>( &Ast.Expr( Id ) ) )
    {
        CollectTailIdentifierNames( Ast, Tern->Then, Out );
        CollectTailIdentifierNames( Ast, Tern->Else, Out );
        return;
    }
    if ( const auto *IfNode = std::get_if<Frontend::If>( &Ast.Expr( Id ) ) )
    {
        CollectTailIdentifierNamesFromBody( Ast, IfNode->Then, Out );
        CollectTailIdentifierNamesFromBody( Ast, IfNode->Else, Out );
        return;
    }
    if ( const auto *CaseNode = std::get_if<Frontend::CaseExpr>( &Ast.Expr( Id ) ) )
    {
        for ( const Frontend::StmtId ClauseId : CaseNode->Clauses )
        {
            const auto &Clause = std::get<Frontend::WhenClause>( Ast.Stmt( ClauseId ) );
            CollectTailIdentifierNamesFromBody( Ast, Clause.Body, Out );
        }
        CollectTailIdentifierNamesFromBody( Ast, CaseNode->ElseBody, Out );
        return;
    }
    if ( const auto *BeginNode = std::get_if<Frontend::BeginExpr>( &Ast.Expr( Id ) ) )
    {
        CollectTailIdentifierNamesFromBody( Ast, BeginNode->Body, Out );
        for ( const Frontend::StmtId RescueId : BeginNode->RescueClauses )
        {
            const auto &Rescue = std::get<Frontend::RescueClause>( Ast.Stmt( RescueId ) );
            CollectTailIdentifierNamesFromBody( Ast, Rescue.Body, Out );
        }
        return;
    }
    // Anything else (Binary, Call, ...) is not a bare passthrough of an
    // existing binding — conservatively still finalized, same as the
    // project's existing "anything more complex than a bare Identifier
    // return is conservative" philosophy.
}

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
