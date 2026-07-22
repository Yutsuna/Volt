#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Pass.hpp"

#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace Volt
{

namespace Sema
{

    namespace
    {

        using namespace Frontend;

        class FunctionalRewriter
        {

        public:

            explicit FunctionalRewriter ( AstContext &InContext ) : Context( InContext )
            {
            }

            void Run ()
            {
                for ( const DeclId Id : Context.TopDecls )
                {
                    WalkDecl( Id );
                }
                for ( const StmtId Id : Context.TopStmts )
                {
                    WalkStmt( Id );
                }
            }

        private:

            void WalkDecl ( DeclId Id )
            {
                if ( not Id.IsValid() )
                {
                    return;
                }
                std::visit(
                    Meta::Overloaded{
                        [&] ( auto &Node ) { WalkFields( Node ); },
                    },
                    Context.Decl( Id ) );
            }

            void WalkStmt ( StmtId Id )
            {
                if ( not Id.IsValid() )
                {
                    return;
                }
                std::visit(
                    Meta::Overloaded{
                        [&] ( auto &Node ) { WalkFields( Node ); },
                    },
                    Context.Stmt( Id ) );
            }

            void WalkExpr ( ExprId &Id )
            {
                if ( not Id.IsValid() )
                {
                    return;
                }

                // Rewrite bottom-up
                std::visit(
                    Meta::Overloaded{
                        [&] ( auto &Node ) { WalkFields( Node ); },
                    },
                    Context.Expr( Id ) );

                const ExprKind Kind = KindOf( Context.Expr( Id ) );
                if ( Kind == ExprKind::Section )
                {
                    Id = LowerSection( Id );
                }
                else if ( Kind == ExprKind::Composition )
                {
                    Id = LowerComposition( Id );
                }
            }

            template <typename NodeType> void WalkFields ( NodeType &Node )
            {
                if constexpr ( Meta::Reflected<NodeType> )
                {
                    Meta::ForEachField( Node,
                                        [&] ( std::string_view, auto &Field )
                                        {
                                            using FieldType = std::remove_reference_t<decltype( Field )>;
                                            if constexpr ( std::is_same_v<FieldType, ExprId> )
                                            {
                                                WalkExpr( Field );
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, ExprList> )
                                            {
                                                for ( ExprId &Child : Field )
                                                {
                                                    WalkExpr( Child );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, StmtId> )
                                            {
                                                WalkStmt( Field );
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, StmtList> )
                                            {
                                                for ( StmtId &Child : Field )
                                                {
                                                    WalkStmt( Child );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, DeclList> )
                                            {
                                                for ( DeclId &Child : Field )
                                                {
                                                    WalkDecl( Child );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, ParamList> )
                                            {
                                                for ( ParamId &Child : Field )
                                                {
                                                    WalkExpr( Context.GetParam( Child ).Default );
                                                }
                                            }
                                        } );
                }
            }

            [[nodiscard]] ExprId LowerSection ( ExprId SectionId )
            {
                const Section Sec = std::get<Section>( Context.Expr( SectionId ) );

                const Symbol ParamName = Context.MakeUniqueSymbol( "fn_tmp" );

                Param P;
                P.Loc             = Sec.Loc;
                P.Name            = ParamName;
                P.DeclType        = TypeId{};
                P.Default         = ExprId{};
                const ParamId PId = Context.Add( P );

                ParamList Params;
                Params.PushBack( PId );

                Identifier IdNode;
                IdNode.Loc          = Sec.Loc;
                IdNode.Name         = ParamName;
                const ExprId IdExpr = Context.Add( IdNode );

                ExprId BodyExpr{};

                if ( Sec.Kind == ESectionKind::InstanceMethod )
                {
                    Frontend::Member M;
                    M.Loc            = Sec.Loc;
                    M.Object         = IdExpr;
                    M.Name           = Sec.Target;
                    const ExprId MId = Context.Add( M );

                    Call C;
                    C.Loc    = Sec.Loc;
                    C.Callee = MId;
                    C.Args   = Sec.Args;
                    for ( std::size_t i = 0; i < Sec.Args.Size(); ++i )
                    {
                        C.ArgNames.PushBack( Symbol{} );
                    }
                    BodyExpr = Context.Add( C );
                }
                else if ( Sec.Kind == ESectionKind::Operator )
                {
                    Frontend::Member M;
                    M.Loc            = Sec.Loc;
                    M.Object         = IdExpr;
                    M.Name           = Sec.Target;
                    const ExprId MId = Context.Add( M );

                    Call C;
                    C.Loc    = Sec.Loc;
                    C.Callee = MId;
                    C.Args   = Sec.Args;
                    for ( std::size_t i = 0; i < Sec.Args.Size(); ++i )
                    {
                        C.ArgNames.PushBack( Symbol{} );
                    }
                    BodyExpr = Context.Add( C );
                }
                else if ( Sec.Kind == ESectionKind::StaticCapture )
                {
                    Call C;
                    C.Loc    = Sec.Loc;
                    C.Callee = Sec.TargetExpr;
                    C.Args.PushBack( IdExpr );
                    C.ArgNames.PushBack( Symbol{} );
                    BodyExpr = Context.Add( C );
                }

                if ( Sec.bNegated )
                {
                    Unary U;
                    U.Loc     = Sec.Loc;
                    U.Op      = TokenKind::Bang;
                    U.Operand = BodyExpr;
                    BodyExpr  = Context.Add( U );
                }

                Lambda L;
                L.Loc        = Sec.Loc;
                L.Params     = Params;
                L.ReturnType = ExprId{};
                L.Body       = BodyExpr;
                return Context.Add( L );
            }

            [[nodiscard]] ExprId LowerComposition ( ExprId CompId )
            {
                const Composition Comp = std::get<Composition>( Context.Expr( CompId ) );

                const Symbol ParamName = Context.MakeUniqueSymbol( "fn_tmp" );

                Param P;
                P.Loc             = Comp.Loc;
                P.Name            = ParamName;
                P.DeclType        = TypeId{};
                P.Default         = ExprId{};
                const ParamId PId = Context.Add( P );

                ParamList Params;
                Params.PushBack( PId );

                Identifier IdNode;
                IdNode.Loc          = Comp.Loc;
                IdNode.Name         = ParamName;
                const ExprId IdExpr = Context.Add( IdNode );

                Call InnerCall;
                InnerCall.Loc    = Comp.Loc;
                InnerCall.Callee = Comp.Lhs;
                InnerCall.Args.PushBack( IdExpr );
                InnerCall.ArgNames.PushBack( Symbol{} );
                const ExprId InnerExpr = Context.Add( InnerCall );

                Call OuterCall;
                OuterCall.Loc    = Comp.Loc;
                OuterCall.Callee = Comp.Rhs;
                OuterCall.Args.PushBack( InnerExpr );
                OuterCall.ArgNames.PushBack( Symbol{} );
                const ExprId OuterExpr = Context.Add( OuterCall );

                Lambda L;
                L.Loc        = Comp.Loc;
                L.Params     = Params;
                L.ReturnType = ExprId{};
                L.Body       = OuterExpr;
                return Context.Add( L );
            }

            AstContext &Context;
        };

    } // namespace

    // Order 8 — rewrite Section and Composition nodes into standard Lambda nodes.
    void FunctionalLowering ( PassContext &Context )
    {
        FunctionalRewriter Rewriter{ Context.Ast };
        Rewriter.Run();
    }

} // namespace Sema

} // namespace Volt
