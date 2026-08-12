#include "ExitPaths.hpp"

#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"

#include <cstdint>
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

namespace Volt::Sema::TypeCheckerPass::Lifetime
{

namespace
{

    // A read-only structural descent over the *expression* fields reachable
    // from a node, stopping at every `If`/`CaseExpr`/`BeginExpr` it finds.
    // Written with `Meta::ForEachField` rather than a per-node switch, so a
    // node added to `Nodes.inl` is walked with no edit here
    // (rules/meta-first.md).
    //
    // `StmtId`/`StmtList` fields are deliberately NOT followed: the only
    // expressions that own statements are the three this stops at, and the
    // one statement that does (`While`) is not reachable from an expression
    // at all. So skipping them costs nothing and guarantees the "outermost
    // only" property the header promises.
    template <typename NodeVariant>
    void ScanBlockFields ( const Frontend::AstContext &Ast, const NodeVariant &Variant, std::vector<Frontend::ExprId> &Out );

    void CollectBlockExprs ( const Frontend::AstContext &Ast, const Frontend::ExprId Id, std::vector<Frontend::ExprId> &Out )
    {
        if ( not Id.IsValid() )
        {
            return;
        }
        const Frontend::ExprNode &Node = Ast.Expr( Id );
        if ( std::holds_alternative<Frontend::If>( Node ) or std::holds_alternative<Frontend::CaseExpr>( Node ) or
             std::holds_alternative<Frontend::BeginExpr>( Node ) )
        {
            Out.push_back( Id );
            return;
        }
        ScanBlockFields( Ast, Node, Out );
    }

    template <typename NodeVariant>
    void ScanBlockFields ( const Frontend::AstContext &Ast, const NodeVariant &Variant, std::vector<Frontend::ExprId> &Out )
    {
        std::visit(
            [&] ( const auto &Node )
            {
                using T = std::remove_cvref_t<decltype( Node )>;
                if constexpr ( not std::is_same_v<T, std::monostate> )
                {
                    Meta::ForEachField( Node,
                                        [&] ( const char *, const auto &Field )
                                        {
                                            using F = std::remove_cvref_t<decltype( Field )>;
                                            if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                            {
                                                CollectBlockExprs( Ast, Field, Out );
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                            {
                                                for ( const Frontend::ExprId Child : Field )
                                                {
                                                    CollectBlockExprs( Ast, Child, Out );
                                                }
                                            }
                                        } );
                }
            },
            Variant );
    }

} // namespace

std::vector<Frontend::ExprId> CollectNestedBlockExprs ( const Frontend::AstContext &Ast, const Frontend::StmtId Id )
{
    std::vector<Frontend::ExprId> Out;
    if ( not Id.IsValid() )
    {
        return Out;
    }
    ScanBlockFields( Ast, Ast.Stmt( Id ), Out );
    return Out;
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
