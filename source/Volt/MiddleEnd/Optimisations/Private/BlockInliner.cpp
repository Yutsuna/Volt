#include "Volt/MiddleEnd/Optimisations/BlockInliner.hpp"

#include "DeclStmtWalker.hpp"
#include "ExprInferencer.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstClone.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"
#include "Volt/MiddleEnd/Optimisations/InlineSummary.hpp"
#include "Volt/MiddleEnd/Resolver/ScopeTable.hpp"

#include <string_view>
#include <unordered_map>
#include <vector>

namespace Volt::MiddleEnd::Optimisations
{

namespace
{

    class BodySubstituter
    {
    public:

        BodySubstituter ( Frontend::AstContext &InAst,
                          const std::unordered_map<Frontend::Symbol, Frontend::Symbol> &InParamMap,
                          Frontend::Symbol InReceiverLocal,
                          Frontend::Symbol InBlockParamName,
                          const Frontend::ParamList &InBlockParams,
                          const Frontend::StmtList &InBlockBody,
                          Volt::Core::SourceRange InCallLoc )
            : Ast( InAst ), ParamMap( InParamMap ), ReceiverLocal( InReceiverLocal ), BlockParamName( InBlockParamName ),
              BlockParams( InBlockParams ), BlockBody( InBlockBody ), CallLoc( InCallLoc )
        {
        }

        Frontend::StmtList TransformStmts ( const Frontend::StmtList &Stmts )
        {
            Frontend::StmtList Result;
            for ( const Frontend::StmtId StmtId : Stmts )
            {
                TransformStmtAppend( StmtId, Result );
            }
            return Result;
        }

    private:

        void TransformStmtAppend ( Frontend::StmtId Id, Frontend::StmtList &OutList )
        {
            if ( not Id.IsValid() )
            {
                return;
            }

            Frontend::StmtNode &Node = Ast.Stmt( Id );

            // Check if this statement is an ExprStmt wrapping a call to the block
            if ( auto *ExprStmtNode = std::get_if<Frontend::ExprStmt>( &Node ) )
            {
                if ( ExprStmtNode->Expr.IsValid() )
                {
                    const Frontend::ExprNode &InnerExpr = Ast.Expr( ExprStmtNode->Expr );
                    if ( const auto *CallNode = std::get_if<Frontend::Call>( &InnerExpr ) )
                    {
                        if ( IsBlockCall( CallNode->Callee ) )
                        {
                            EmitInlinedBlock( CallNode->Args, OutList );
                            return;
                        }
                    }
                }
            }

            // If it's a while loop, recursively transform its body
            if ( auto *WhileNode = std::get_if<Frontend::While>( &Node ) )
            {
                WhileNode->Cond = TransformExpr( WhileNode->Cond );
                WhileNode->Body = TransformStmts( WhileNode->Body );
                OutList.PushBack( Id );
                return;
            }

            // General statement traversal
            std::visit(
                [&] ( auto &Concrete )
                {
                    using T = std::decay_t<decltype( Concrete )>;
                    if constexpr ( Meta::Reflected<T> )
                    {
                        Meta::ForEachField( Concrete,
                                            [&] ( std::string_view, auto &Field )
                                            {
                                                using FieldType = std::remove_cvref_t<decltype( Field )>;
                                                if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                                                {
                                                    Field = TransformExpr( Field );
                                                }
                                                else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
                                                {
                                                    for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
                                                    {
                                                        Field[Index] = TransformExpr( Field[Index] );
                                                    }
                                                }
                                                else if constexpr ( std::is_same_v<FieldType, Frontend::StmtList> )
                                                {
                                                    Field = TransformStmts( Field );
                                                }
                                            } );
                    }
                },
                Node );

            OutList.PushBack( Id );
        }

        Frontend::ExprId TransformExpr ( Frontend::ExprId Id )
        {
            if ( not Id.IsValid() )
            {
                return Frontend::ExprId{};
            }

            Frontend::ExprNode &Node = Ast.Expr( Id );

            if ( auto *Ident = std::get_if<Frontend::Identifier>( &Node ) )
            {
                if ( const auto It = ParamMap.find( Ident->Name ); It != ParamMap.end() )
                {
                    Ident->Name = It->second;
                }
                return Id;
            }

            if ( std::holds_alternative<Frontend::SelfExpr>( Node ) )
            {
                if ( ReceiverLocal.IsValid() )
                {
                    return Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = CallLoc, .Name = ReceiverLocal } } );
                }
            }

            if ( auto *Ivar = std::get_if<Frontend::InstanceVar>( &Node ) )
            {
                if ( ReceiverLocal.IsValid() )
                {
                    const Frontend::ExprId RecObj =
                        Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = CallLoc, .Name = ReceiverLocal } } );
                    return Ast.Add(
                        Frontend::ExprNode{ Frontend::Member{ .Loc = CallLoc, .Object = RecObj, .Name = Ivar->Name } } );
                }
            }

            std::visit(
                [&] ( auto &Concrete )
                {
                    using T = std::decay_t<decltype( Concrete )>;
                    if constexpr ( Meta::Reflected<T> )
                    {
                        Meta::ForEachField( Concrete,
                                            [&] ( std::string_view, auto &Field )
                                            {
                                                using FieldType = std::remove_cvref_t<decltype( Field )>;
                                                if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                                                {
                                                    Field = TransformExpr( Field );
                                                }
                                                else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
                                                {
                                                    for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
                                                    {
                                                        Field[Index] = TransformExpr( Field[Index] );
                                                    }
                                                }
                                            } );
                    }
                },
                Node );

            return Id;
        }

        bool IsBlockCall ( Frontend::ExprId CalleeId ) const
        {
            if ( not CalleeId.IsValid() or not BlockParamName.IsValid() )
            {
                return false;
            }

            const Frontend::ExprNode &CalleeExpr = Ast.Expr( CalleeId );
            if ( const auto *Ident = std::get_if<Frontend::Identifier>( &CalleeExpr ) )
            {
                return Ident->Name == BlockParamName;
            }
            if ( const auto *Mem = std::get_if<Frontend::Member>( &CalleeExpr ) )
            {
                if ( Mem->Object.IsValid() and Ast.Text( Mem->Name ) == "call" )
                {
                    const Frontend::ExprNode &ObjExpr = Ast.Expr( Mem->Object );
                    if ( const auto *ObjIdent = std::get_if<Frontend::Identifier>( &ObjExpr ) )
                    {
                        return ObjIdent->Name == BlockParamName;
                    }
                }
            }
            return false;
        }

        void EmitInlinedBlock ( const Frontend::ExprList &CallArgs, Frontend::StmtList &OutList )
        {
            // Bind block parameters to CallArgs
            for ( std::size_t Index = 0; Index < BlockParams.Size(); ++Index )
            {
                const Frontend::Param &ParamNode = Ast.GetParam( BlockParams[Index] );
                const Frontend::ExprId ArgVal = Index < CallArgs.Size() ? TransformExpr( CallArgs[Index] ) : Frontend::ExprId{};

                OutList.PushBack( Ast.Add( Frontend::StmtNode{
                    Frontend::LocalDecl{ .Loc = CallLoc, .Name = ParamNode.Name, .DeclType = {}, .Init = ArgVal } } ) );
            }

            // Append cloned block body statements
            for ( const Frontend::StmtId Stmt : BlockBody )
            {
                OutList.PushBack( Stmt );
            }
        }

        Frontend::AstContext &Ast;
        const std::unordered_map<Frontend::Symbol, Frontend::Symbol> &ParamMap;
        Frontend::Symbol ReceiverLocal;
        Frontend::Symbol BlockParamName;
        const Frontend::ParamList &BlockParams;
        const Frontend::StmtList &BlockBody;
        Volt::Core::SourceRange CallLoc;
    };

} // namespace

void InlineBlockCalls ( Analysis::TypeCheckerContext &State )
{
    Frontend::AstContext &Ast      = State.Ctx.Ast;
    const std::size_t InitialCount = Ast.ExprCount();

    for ( std::size_t Index = 0; Index < InitialCount; ++Index )
    {
        const Frontend::ExprId CallId{ static_cast<Frontend::ExprId::ValueType>( Index ) };
        const Frontend::ExprNode &Expr = Ast.Expr( CallId );
        const auto *CallNode           = std::get_if<Frontend::Call>( &Expr );
        if ( CallNode == nullptr or not CallNode->BlockArg.IsValid() )
        {
            continue;
        }

        const Frontend::ExprNode &BlockArgNode = Ast.Expr( CallNode->BlockArg );
        Frontend::ParamList BlockParams;
        Frontend::StmtList BlockBody;
        if ( const auto *Blk = std::get_if<Frontend::Block>( &BlockArgNode ) )
        {
            BlockParams = Blk->Params;
            BlockBody   = Blk->Body;
        }
        else if ( const auto *Lam = std::get_if<Frontend::Lambda>( &BlockArgNode ) )
        {
            BlockParams = Lam->Params;
            BlockBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Lam->Loc, .Expr = Lam->Body } } ) );
        }
        else
        {
            continue;
        }

        const Resolver::ScopeId BlockScope = State.Ctx.Scopes.ScopeOfExpr( CallNode->BlockArg );
        if ( BlockScope.IsValid() and State.Ctx.Scopes.Escapes( BlockScope ) )
        {
            ++State.Ctx.Stats.BlockCallsRejected;
            continue;
        }

        const auto CalleeIt = State.CalleeResolution.find( CallNode->Callee.Value );
        if ( CalleeIt == State.CalleeResolution.end() )
        {
            ++State.Ctx.Stats.BlockCallsRejected;
            continue;
        }

        const Analysis::Resolution &Res = CalleeIt->second;
        if ( Res.Decl == nullptr or Res.bIndirect or Res.bDynamicDispatch or Res.bConstructs or
             Res.MachineConversion != IR::EMachineConversion::None )
        {
            ++State.Ctx.Stats.BlockCallsRejected;
            continue;
        }

        const TypeSystem::Member *CalleeMember = Res.Decl;
        if ( CalleeMember->InlineVerdict == TypeSystem::EInlineVerdict::Never )
        {
            ++State.Ctx.Stats.BlockCallsRejected;
            continue;
        }

        const std::size_t DeclUnit = CalleeMember->Unit;
        if ( DeclUnit >= State.Ctx.AllUnits.size() or State.Ctx.AllUnits[DeclUnit] == nullptr )
        {
            ++State.Ctx.Stats.BlockCallsRejected;
            continue;
        }

        const Frontend::AstContext &CalleeAst = *State.Ctx.AllUnits[DeclUnit];
        const auto *MethodDecl                = std::get_if<Frontend::Method>( &CalleeAst.Decl( CalleeMember->Decl ) );
        if ( MethodDecl == nullptr )
        {
            ++State.Ctx.Stats.BlockCallsRejected;
            continue;
        }

        const InlineSummary Summary = SummarizeMethod( CalleeAst, *MethodDecl, State.Ctx.Types, Res.Owner );
        if ( Summary.bHasEarlyReturn or Summary.bTouchesNonPublicMember or not Summary.bHasBlockParam or
             Summary.CallSites != Summary.BlockCalls or Summary.BlockCalls != 1 )
        {
            ++State.Ctx.Stats.BlockCallsRejected;
            continue;
        }

        Frontend::Symbol CalleeBlockParamName;
        for ( const Frontend::ParamId PId : MethodDecl->Params )
        {
            const Frontend::Param &P = CalleeAst.GetParam( PId );
            if ( P.bIsBlock )
            {
                CalleeBlockParamName = Ast.Strings().Intern( CalleeAst.Text( P.Name ) );
                break;
            }
        }

        Frontend::StmtList SpliceStmts;
        Frontend::Symbol ReceiverLocal;

        if ( not CalleeMember->bSelf and Res.Owner.IsValid() )
        {
            Frontend::ExprId ReceiverExprId;
            const Frontend::ExprNode &CalleeExpr = Ast.Expr( CallNode->Callee );
            if ( const auto *Mem = std::get_if<Frontend::Member>( &CalleeExpr ) )
            {
                ReceiverExprId = Mem->Object;
            }
            else
            {
                ReceiverExprId = Ast.Add( Frontend::ExprNode{ Frontend::SelfExpr{ .Loc = CallNode->Loc } } );
            }

            ReceiverLocal = Ast.MakeUniqueSymbol( "__in_rec" );
            SpliceStmts.PushBack( Ast.Add( Frontend::StmtNode{
                Frontend::LocalDecl{ .Loc = CallNode->Loc, .Name = ReceiverLocal, .DeclType = {}, .Init = ReceiverExprId } } ) );
        }

        std::unordered_map<Frontend::Symbol, Frontend::Symbol> ParamMap;
        std::size_t ArgIndex = 0;
        for ( const Frontend::ParamId PId : MethodDecl->Params )
        {
            const Frontend::Param &P = CalleeAst.GetParam( PId );
            if ( P.bIsBlock )
            {
                continue;
            }
            const Frontend::Symbol PName = Ast.Strings().Intern( CalleeAst.Text( P.Name ) );
            Frontend::ExprId ArgVal;
            if ( ArgIndex < CallNode->Args.Size() )
            {
                ArgVal = CallNode->Args[ArgIndex++];
            }
            else if ( P.Default.IsValid() )
            {
                ArgVal = Frontend::CloneExpr( CalleeAst, Ast, P.Default );
            }

            const Frontend::Symbol ArgLocal = Ast.MakeUniqueSymbol( "__in_arg" );
            SpliceStmts.PushBack( Ast.Add( Frontend::StmtNode{
                Frontend::LocalDecl{ .Loc = CallNode->Loc, .Name = ArgLocal, .DeclType = {}, .Init = ArgVal } } ) );
            ParamMap.emplace( PName, ArgLocal );
        }

        Frontend::StmtList ClonedMethodBody = Frontend::CloneStmtList( CalleeAst, Ast, MethodDecl->Body );

        BodySubstituter Substituter( Ast, ParamMap, ReceiverLocal, CalleeBlockParamName, BlockParams, BlockBody, CallNode->Loc );

        Frontend::StmtList InlinedBody = Substituter.TransformStmts( ClonedMethodBody );
        for ( const Frontend::StmtId Stmt : InlinedBody )
        {
            SpliceStmts.PushBack( Stmt );
        }

        Frontend::ExprId NilResult = Ast.Add( Frontend::ExprNode{ Frontend::NilLiteral{ .Loc = CallNode->Loc } } );
        SpliceStmts.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = CallNode->Loc, .Expr = NilResult } } ) );

        Frontend::BeginExpr BeginNode{ .Loc = CallNode->Loc, .Body = SpliceStmts, .RescueClauses = {}, .EnsureBody = {} };

        Analysis::WalkStmts( State, SpliceStmts );
        Ast.Expr( CallId ) = Frontend::ExprNode{ std::move( BeginNode ) };
        Analysis::InferExpr( State, CallId );
        ++State.Ctx.Stats.BlockCallsInlined;
    }
}

} // namespace Volt::MiddleEnd::Optimisations
