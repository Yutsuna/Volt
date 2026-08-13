// CaseLowering.cpp — order 22. Desugars and normalizes `case when [then]` expressions:
//
// 1. Expands `.method(args...)` (DotCall) into `target.method(args...)` (or `self.method(args...)` if no target).
// 2. Desugars `when pattern1, pattern2` by transforming each pattern into `pattern === target` (using TokenKind::TripleEq).
// 3. Leaves `CaseExpr` normalized for TypeChecker and downstream LLVM jump-table / decision tree generation.

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

namespace
{

using namespace Volt;

class CaseRewriter
{

public:

    explicit CaseRewriter ( Frontend::AstContext &InContext ) : Context( InContext )
    {
    }

    std::size_t Run ()
    {
        const std::size_t OriginalExprCount = Context.ExprCount();
        std::size_t Rewritten               = 0;

        for ( std::size_t Index = 0; Index < OriginalExprCount; ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            if ( KindOf( Context.Expr( Id ) ) == Frontend::ExprKind::CaseExpr )
            {
                LowerCaseNode( Id );
                ++Rewritten;
            }
        }

        return Rewritten;
    }

private:

    void LowerCaseNode ( Frontend::ExprId CaseId )
    {
        Frontend::CaseExpr Case = std::get<Frontend::CaseExpr>( Context.Expr( CaseId ) );

        const Frontend::ExprId TargetId = Case.Target;
        const bool bHasTarget           = TargetId.IsValid();

        Frontend::ExprId SelfTarget{};
        if ( not bHasTarget )
        {
            Frontend::SelfExpr SelfNode;
            SelfNode.Loc = Case.Loc;
            SelfTarget   = Context.Add( Frontend::ExprNode{ SelfNode } );
        }

        for ( const Frontend::StmtId ClauseId : Case.Clauses )
        {
            if ( not ClauseId.IsValid() or KindOf( Context.Stmt( ClauseId ) ) != Frontend::StmtKind::WhenClause )
            {
                continue;
            }

            Frontend::WhenClause &Clause = std::get<Frontend::WhenClause>( Context.Stmt( ClauseId ) );

            Frontend::ExprList DesugaredPatterns;
            for ( const Frontend::ExprId PatternId : Clause.Patterns )
            {
                if ( not PatternId.IsValid() )
                {
                    continue;
                }

                Frontend::ExprId ProcessedPattern = PatternId;

                if ( KindOf( Context.Expr( PatternId ) ) == Frontend::ExprKind::DotCall )
                {
                    const Frontend::DotCall Dot = std::get<Frontend::DotCall>( Context.Expr( PatternId ) );

                    Frontend::Member Mem;
                    Mem.Loc                      = Dot.Loc;
                    Mem.Object                   = bHasTarget ? TargetId : SelfTarget;
                    Mem.Name                     = Dot.Method;
                    const Frontend::ExprId MemId = Context.Add( Frontend::ExprNode{ Mem } );

                    Frontend::Call CallNode;
                    CallNode.Loc      = Dot.Loc;
                    CallNode.Callee   = MemId;
                    CallNode.Args     = Dot.Args;
                    CallNode.ArgNames = Dot.ArgNames;
                    ProcessedPattern  = Context.Add( Frontend::ExprNode{ CallNode } );
                }
                else if ( bHasTarget )
                {
                    const Volt::Core::SourceRange PatternLoc = std::visit(
                        [] ( const auto &Node ) -> Volt::Core::SourceRange
                        {
                            using T = std::decay_t<decltype( Node )>;
                            if constexpr ( std::is_same_v<T, std::monostate> )
                            {
                                return {};
                            }
                            else
                            {
                                return Node.Loc;
                            }
                        },
                        Context.Expr( PatternId ) );

                    Frontend::Binary TripleEqNode;
                    TripleEqNode.Loc = PatternLoc;
                    TripleEqNode.Op  = Frontend::TokenKind::TripleEq;
                    TripleEqNode.Lhs = PatternId;
                    TripleEqNode.Rhs = TargetId;
                    ProcessedPattern = Context.Add( Frontend::ExprNode{ TripleEqNode } );
                }

                DesugaredPatterns.PushBack( ProcessedPattern );
            }

            Clause.Patterns = std::move( DesugaredPatterns );
        }

        Case.Scrutinee = TargetId;
        Case.Target    = Frontend::ExprId{};

        Context.Expr( CaseId ) = Frontend::ExprNode{ std::move( Case ) };
    }

    Frontend::AstContext &Context;
};

} // namespace

void Volt::MiddleEnd::Lowering::CaseLowering ( Core::PassContext &Context )
{
    Context.Stats.CaseLowered += CaseRewriter( Context.Ast ).Run();
}
