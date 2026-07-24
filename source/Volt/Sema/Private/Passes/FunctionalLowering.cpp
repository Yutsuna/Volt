#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Sema/Pass.hpp"

#include <string_view>
#include <type_traits>
#include <variant>

namespace
{

using namespace Volt;

class FunctionalRewriter
{

public:

    explicit FunctionalRewriter ( Frontend::AstContext &InContext ) : Context( InContext )
    {
    }

    void Run ()
    {
        for ( const Frontend::DeclId Id : Context.TopDecls )
        {
            WalkDecl( Id );
        }
        for ( const Frontend::StmtId Id : Context.TopStmts )
        {
            WalkStmt( Id );
        }
    }

private:

    void WalkDecl ( Frontend::DeclId Id )
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

    void WalkStmt ( Frontend::StmtId Id )
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

    void WalkExpr ( Frontend::ExprId &Id )
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

        const Frontend::ExprKind Kind = KindOf( Context.Expr( Id ) );
        if ( Kind == Frontend::ExprKind::Section )
        {
            Id = LowerSection( Id );
        }
        else if ( Kind == Frontend::ExprKind::Composition )
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
                                    if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                                    {
                                        WalkExpr( Field );
                                    }
                                    else if constexpr ( std::is_same_v<FieldType, Volt::Frontend::ExprList> )
                                    {
                                        for ( Frontend::ExprId &Child : Field )
                                        {
                                            WalkExpr( Child );
                                        }
                                    }
                                    else if constexpr ( std::is_same_v<FieldType, Frontend::StmtId> )
                                    {
                                        WalkStmt( Field );
                                    }
                                    else if constexpr ( std::is_same_v<FieldType, Volt::Frontend::StmtList> )
                                    {
                                        for ( Frontend::StmtId &Child : Field )
                                        {
                                            WalkStmt( Child );
                                        }
                                    }
                                    else if constexpr ( std::is_same_v<FieldType, Volt::Frontend::DeclList> )
                                    {
                                        for ( Frontend::DeclId &Child : Field )
                                        {
                                            WalkDecl( Child );
                                        }
                                    }
                                    else if constexpr ( std::is_same_v<FieldType, Volt::Frontend::ParamList> )
                                    {
                                        for ( Frontend::ParamId &Child : Field )
                                        {
                                            WalkExpr( Context.GetParam( Child ).Default );
                                        }
                                    }
                                } );
        }
    }

    // WARN: this is so hardcoded
    // TODO: refactor to avoid redundancy

    [[nodiscard]] Frontend::ExprId LowerSection ( Frontend::ExprId SectionId )
    {
        const Volt::Frontend::Section Sec  = std::get<Volt::Frontend::Section>( Context.Expr( SectionId ) );
        const Volt::Core::Symbol ParamName = Context.MakeUniqueSymbol( "fn_tmp" );

        Frontend::Param Param;
        Param.Loc                         = Sec.Loc;
        Param.Name                        = ParamName;
        Param.DeclType                    = Frontend::TypeId{};
        Param.Default                     = Frontend::ExprId{};
        const Volt::Frontend::ParamId PId = Context.Add( Param );

        Frontend::ParamList Params;
        Params.PushBack( PId );

        Frontend::Identifier IdNode;
        IdNode.Loc                    = Sec.Loc;
        IdNode.Name                   = ParamName;
        const Frontend::ExprId IdExpr = Context.Add( IdNode );

        Frontend::ExprId BodyExpr{};

        if ( Sec.Kind == Frontend::ESectionKind::InstanceMethod )
        {
            Frontend::Member Mem;
            Mem.Loc                    = Sec.Loc;
            Mem.Object                 = IdExpr;
            Mem.Name                   = Sec.Target;
            const Frontend::ExprId MId = Context.Add( Mem );

            Frontend::Call Cal;
            Cal.Loc    = Sec.Loc;
            Cal.Callee = MId;
            Cal.Args   = Sec.Args;
            for ( std::size_t Idx = 0; Idx < Sec.Args.Size(); ++Idx )
            {
                Cal.ArgNames.PushBack( Frontend::Symbol{} );
            }
            BodyExpr = Context.Add( Cal );
        }
        else if ( Sec.Kind == Frontend::ESectionKind::Operator )
        {
            Frontend::Member Mem;
            Mem.Loc                    = Sec.Loc;
            Mem.Object                 = IdExpr;
            Mem.Name                   = Sec.Target;
            const Frontend::ExprId MId = Context.Add( Mem );

            Frontend::Call Cal;
            Cal.Loc    = Sec.Loc;
            Cal.Callee = MId;
            Cal.Args   = Sec.Args;
            for ( std::size_t Idx = 0; Idx < Sec.Args.Size(); ++Idx )
            {
                Cal.ArgNames.PushBack( Frontend::Symbol{} );
            }
            BodyExpr = Context.Add( Cal );
        }
        else if ( Sec.Kind == Frontend::ESectionKind::StaticCapture )
        {
            Frontend::Call Cal;
            Cal.Loc    = Sec.Loc;
            Cal.Callee = Sec.TargetExpr;
            Cal.Args.PushBack( IdExpr );
            Cal.ArgNames.PushBack( Frontend::Symbol{} );
            BodyExpr = Context.Add( Cal );
        }

        if ( Sec.bNegated )
        {
            Frontend::Unary Un;
            Un.Loc     = Sec.Loc;
            Un.Op      = Frontend::TokenKind::Bang;
            Un.Operand = BodyExpr;
            BodyExpr   = Context.Add( Un );
        }

        Frontend::Lambda Lam;
        Lam.Loc        = Sec.Loc;
        Lam.Params     = Params;
        Lam.ReturnType = Frontend::ExprId{};
        Lam.Body       = BodyExpr;
        return Context.Add( Lam );
    }

    [[nodiscard]] Frontend::ExprId LowerComposition ( Frontend::ExprId CompId )
    {
        const Frontend::Composition Comp = std::get<Frontend::Composition>( Context.Expr( CompId ) );

        const Frontend::Symbol ParamName = Context.MakeUniqueSymbol( "fn_tmp" );

        Frontend::Param Param;
        Param.Loc                         = Comp.Loc;
        Param.Name                        = ParamName;
        Param.DeclType                    = Frontend::TypeId{};
        Param.Default                     = Frontend::ExprId{};
        const Volt::Frontend::ParamId PId = Context.Add( Param );

        Frontend::ParamList Params;
        Params.PushBack( PId );

        Frontend::Identifier IdNode;
        IdNode.Loc                    = Comp.Loc;
        IdNode.Name                   = ParamName;
        const Frontend::ExprId IdExpr = Context.Add( IdNode );

        Frontend::Call InnerCall;
        InnerCall.Loc    = Comp.Loc;
        InnerCall.Callee = Comp.Lhs;
        InnerCall.Args.PushBack( IdExpr );
        InnerCall.ArgNames.PushBack( Frontend::Symbol{} );
        const Frontend::ExprId InnerExpr = Context.Add( InnerCall );

        Frontend::Call OuterCall;
        OuterCall.Loc    = Comp.Loc;
        OuterCall.Callee = Comp.Rhs;
        OuterCall.Args.PushBack( InnerExpr );
        OuterCall.ArgNames.PushBack( Frontend::Symbol{} );
        const Frontend::ExprId OuterExpr = Context.Add( OuterCall );

        Frontend::Lambda Lam;
        Lam.Loc        = Comp.Loc;
        Lam.Params     = Params;
        Lam.ReturnType = Frontend::ExprId{};
        Lam.Body       = OuterExpr;
        return Context.Add( Lam );
    }

    Frontend::AstContext &Context;
};

} // namespace

// Order 8 — rewrite Section and Composition nodes into standard Lambda nodes.
void Volt::Sema::FunctionalLowering ( Volt::Sema::PassContext &Context )
{
    FunctionalRewriter Rewriter{ Context.Ast };
    Rewriter.Run();
}
