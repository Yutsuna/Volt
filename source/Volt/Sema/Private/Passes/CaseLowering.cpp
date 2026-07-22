// CaseLowering.cpp — order 22. Desugars and normalizes `case when [then]` expressions:
//
// 1. Expands `.method(args...)` (DotCall) into `target.method(args...)` (or `self.method(args...)` if no target).
// 2. Desugars `when pattern1, pattern2` by transforming each pattern into `pattern === target` (using TokenKind::TripleEq).
// 3. Leaves `CaseExpr` normalized for TypeChecker and downstream LLVM jump-table / decision tree generation.

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Pass.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

namespace Volt
{

namespace Sema
{

    namespace
    {

        using namespace Frontend;

        class CaseRewriter
        {

        public:

            explicit CaseRewriter ( AstContext &InContext ) : Context( InContext )
            {
            }

            std::size_t Run ()
            {
                const std::size_t OriginalExprCount = Context.ExprCount();
                std::size_t Rewritten               = 0;

                for ( std::size_t Index = 0; Index < OriginalExprCount; ++Index )
                {
                    const ExprId Id{ static_cast<ExprId::ValueType>( Index ) };
                    if ( KindOf( Context.Expr( Id ) ) == ExprKind::CaseExpr )
                    {
                        LowerCaseNode( Id );
                        ++Rewritten;
                    }
                }

                return Rewritten;
            }

        private:

            void LowerCaseNode ( ExprId CaseId )
            {
                CaseExpr Case = std::get<CaseExpr>( Context.Expr( CaseId ) );

                const ExprId TargetId = Case.Target;
                const bool bHasTarget = TargetId.IsValid();

                ExprId SelfTarget{};
                if ( !bHasTarget )
                {
                    SelfExpr SelfNode;
                    SelfNode.Loc = Case.Loc;
                    SelfTarget   = Context.Add( ExprNode{ SelfNode } );
                }

                for ( const StmtId ClauseId : Case.Clauses )
                {
                    if ( !ClauseId.IsValid() or KindOf( Context.Stmt( ClauseId ) ) != StmtKind::WhenClause )
                    {
                        continue;
                    }

                    WhenClause &Clause = std::get<WhenClause>( Context.Stmt( ClauseId ) );

                    ExprList DesugaredPatterns;
                    for ( const ExprId PatternId : Clause.Patterns )
                    {
                        if ( !PatternId.IsValid() )
                        {
                            continue;
                        }

                        ExprId ProcessedPattern = PatternId;

                        if ( KindOf( Context.Expr( PatternId ) ) == ExprKind::DotCall )
                        {
                            const DotCall &Dot = std::get<DotCall>( Context.Expr( PatternId ) );

                            Frontend::Member Mem;
                            Mem.Loc            = Dot.Loc;
                            Mem.Object         = bHasTarget ? TargetId : SelfTarget;
                            Mem.Name           = Dot.Method;
                            const ExprId MemId = Context.Add( ExprNode{ Mem } );

                            Call CallNode;
                            CallNode.Loc      = Dot.Loc;
                            CallNode.Callee   = MemId;
                            CallNode.Args     = Dot.Args;
                            CallNode.ArgNames = Dot.ArgNames;
                            ProcessedPattern  = Context.Add( ExprNode{ CallNode } );
                        }
                        else if ( bHasTarget )
                        {
                            const Core::SourceRange PatternLoc = std::visit(
                                [] ( const auto &N ) -> Core::SourceRange
                                {
                                    using T = std::decay_t<decltype( N )>;
                                    if constexpr ( std::is_same_v<T, std::monostate> )
                                    {
                                        return {};
                                    }
                                    else
                                    {
                                        return N.Loc;
                                    }
                                },
                                Context.Expr( PatternId ) );

                            Binary TripleEqNode;
                            TripleEqNode.Loc = PatternLoc;
                            TripleEqNode.Op  = TokenKind::TripleEq;
                            TripleEqNode.Lhs = PatternId;
                            TripleEqNode.Rhs = TargetId;
                            ProcessedPattern = Context.Add( ExprNode{ TripleEqNode } );
                        }

                        DesugaredPatterns.PushBack( ProcessedPattern );
                    }

                    Clause.Patterns = std::move( DesugaredPatterns );
                }

                Context.Expr( CaseId ) = ExprNode{ std::move( Case ) };
            }

            AstContext &Context;
        };

    } // namespace

    void CaseLowering ( PassContext &Context )
    {
        Context.Stats.CaseLowered += CaseRewriter( Context.Ast ).Run();
    }

} // namespace Sema

} // namespace Volt
