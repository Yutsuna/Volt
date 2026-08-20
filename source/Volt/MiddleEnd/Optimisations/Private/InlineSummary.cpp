#include "Volt/MiddleEnd/Optimisations/InlineSummary.hpp"

#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"

#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace Volt::MiddleEnd::Optimisations
{

using Core::Symbol;

namespace
{

    class SummaryWalker
    {
    public:

        SummaryWalker ( const Frontend::AstContext &InAst,
                        const Frontend::Method &InMethod,
                        const TypeSystem::TypeStore &InTypes,
                        TypeSystem::NominalId InOwner )
            : Ast( InAst ), Method( InMethod ), Types( InTypes ), Owner( InOwner )
        {
            MethodNameText = Ast.Text( Method.Name );
            for ( const Frontend::ParamId PId : Method.Params )
            {
                const Frontend::Param &ParamNode = Ast.GetParam( PId );
                if ( ParamNode.bIsBlock )
                {
                    Summary.bHasBlockParam = true;
                    BlockParamText         = Ast.Text( ParamNode.Name );
                    break;
                }
            }
        }

        InlineSummary Run ()
        {
            const std::size_t StmtCount = Method.Body.Size();
            for ( std::size_t Index = 0; Index < StmtCount; ++Index )
            {
                const bool bIsFinal = ( Index + 1 == StmtCount );
                WalkStmt( Method.Body[Index], bIsFinal );
            }
            return Summary;
        }

    private:

        void WalkExpr ( Frontend::ExprId Id )
        {
            if ( not Id.IsValid() )
            {
                return;
            }
            ++Summary.NodeCount;

            const Frontend::ExprNode &Expr = Ast.Expr( Id );
            std::visit(
                [&] ( const auto &Concrete )
                {
                    using T = std::decay_t<decltype( Concrete )>;
                    if constexpr ( std::is_same_v<T, Frontend::RaiseExpr> )
                    {
                        Summary.bHasRaise = true;
                    }
                    else if constexpr ( std::is_same_v<T, Frontend::Call> )
                    {
                        ++Summary.CallSites;
                        InspectCall( Concrete.Callee );
                    }
                    else if constexpr ( std::is_same_v<T, Frontend::DotCall> )
                    {
                        ++Summary.CallSites;
                    }
                    else if constexpr ( std::is_same_v<T, Frontend::InstanceVar> )
                    {
                        if ( not Owner.IsValid() )
                        {
                            Summary.bTouchesNonPublicMember = true;
                        }
                        else
                        {
                            const std::string_view IvarText = Ast.Text( Concrete.Name );
                            bool bHasPublicAccessor         = false;
                            for ( const TypeSystem::Member &M : Types.Type( Owner ).Members )
                            {
                                if ( Types.Text( M.Name ) == IvarText and ( M.Visibility == Frontend::EVisibility::Public or
                                                                            M.Visibility == Frontend::EVisibility::None ) )
                                {
                                    bHasPublicAccessor = true;
                                    break;
                                }
                            }
                            if ( not bHasPublicAccessor )
                            {
                                Summary.bTouchesNonPublicMember = true;
                            }
                        }
                    }

                    WalkChildren( Concrete );
                },
                Expr );
        }

        void InspectCall ( Frontend::ExprId CalleeId )
        {
            if ( not CalleeId.IsValid() )
            {
                return;
            }

            const Frontend::ExprNode &CalleeExpr = Ast.Expr( CalleeId );
            if ( const auto *Ident = std::get_if<Frontend::Identifier>( &CalleeExpr ) )
            {
                const std::string_view Name = Ast.Text( Ident->Name );
                if ( Name == MethodNameText )
                {
                    Summary.bSelfRecursive = true;
                }
                if ( Summary.bHasBlockParam and Name == BlockParamText )
                {
                    ++Summary.BlockCalls;
                }
            }
            else if ( const auto *Mem = std::get_if<Frontend::Member>( &CalleeExpr ) )
            {
                const std::string_view MemName = Ast.Text( Mem->Name );
                if ( Mem->Object.IsValid() )
                {
                    const Frontend::ExprNode &TargetExpr = Ast.Expr( Mem->Object );
                    if ( std::holds_alternative<Frontend::SelfExpr>( TargetExpr ) and MemName == MethodNameText )
                    {
                        Summary.bSelfRecursive = true;
                    }
                    if ( Summary.bHasBlockParam )
                    {
                        if ( const auto *TargetIdent = std::get_if<Frontend::Identifier>( &TargetExpr ) )
                        {
                            if ( Ast.Text( TargetIdent->Name ) == BlockParamText and MemName == "call" )
                            {
                                ++Summary.BlockCalls;
                            }
                        }
                    }
                }
            }
        }

        void WalkStmt ( Frontend::StmtId Id, bool bIsFinalTopLevel )
        {
            if ( not Id.IsValid() )
            {
                return;
            }
            ++Summary.NodeCount;

            const Frontend::StmtNode &Stmt = Ast.Stmt( Id );
            std::visit(
                [&] ( const auto &Concrete )
                {
                    using T = std::decay_t<decltype( Concrete )>;
                    if constexpr ( std::is_same_v<T, Frontend::Return> )
                    {
                        if ( not bIsFinalTopLevel )
                        {
                            Summary.bHasEarlyReturn = true;
                        }
                    }
                    else if constexpr ( std::is_same_v<T, Frontend::While> )
                    {
                        Summary.bHasLoop = true;
                    }
                    else if constexpr ( std::is_same_v<T, Frontend::RescueClause> )
                    {
                        Summary.bHasRaise = true;
                    }

                    WalkChildren( Concrete );
                },
                Stmt );
        }

        template <typename NodeType> void WalkChildren ( const NodeType &Node )
        {
            if constexpr ( Meta::Reflected<NodeType> )
            {
                Meta::ForEachField( Node,
                                    [&] ( std::string_view, const auto &Field )
                                    {
                                        using FieldType = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                                        {
                                            WalkExpr( Field );
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                WalkExpr( Child );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, Frontend::StmtId> )
                                        {
                                            WalkStmt( Field, /*bIsFinalTopLevel=*/false );
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, Frontend::StmtList> )
                                        {
                                            for ( const Frontend::StmtId Child : Field )
                                            {
                                                WalkStmt( Child, /*bIsFinalTopLevel=*/false );
                                            }
                                        }
                                    } );
            }
        }

        const Frontend::AstContext &Ast;
        const Frontend::Method &Method;
        const TypeSystem::TypeStore &Types;
        TypeSystem::NominalId Owner;
        std::string_view MethodNameText;
        std::string_view BlockParamText;
        InlineSummary Summary;
    };

} // namespace

InlineSummary SummarizeMethod ( const Frontend::AstContext &Ast,
                                const Frontend::Method &MethodNode,
                                const TypeSystem::TypeStore &Types,
                                TypeSystem::NominalId Owner )
{
    SummaryWalker Walker( Ast, MethodNode, Types, Owner );
    return Walker.Run();
}

EInlineVerdict VerdictOf ( const InlineSummary &Summary, bool bAbstract, bool bExternal )
{
    if ( bAbstract or bExternal or Summary.bSelfRecursive or Summary.bHasRaise or Summary.bHasEarlyReturn or
         Summary.NodeCount > 200 )
    {
        return EInlineVerdict::Never;
    }

    if ( Summary.NodeCount <= 12 and not Summary.bHasLoop and Summary.CallSites <= 1 )
    {
        return EInlineVerdict::Always;
    }

    if ( Summary.NodeCount <= 60 )
    {
        return EInlineVerdict::Hint;
    }

    return EInlineVerdict::Never;
}

void AnalyzeInlineCandidates ( std::span<const Frontend::AstContext *const> Units, TypeSystem::TypeStore &Store )
{
    for ( std::size_t TypeIdx = 0; TypeIdx < Store.TypeCount(); ++TypeIdx )
    {
        const TypeSystem::NominalId Id{ static_cast<TypeSystem::NominalId::ValueType>( TypeIdx ) };
        auto Members = Store.MutableMembers( Id );
        for ( TypeSystem::Member &Entry : Members )
        {
            if ( Entry.Kind != TypeSystem::EMemberKind::Method )
            {
                continue;
            }
            if ( Entry.bAbstract or Entry.ExternSymbol.IsValid() )
            {
                Entry.InlineVerdict = EInlineVerdict::Never;
                continue;
            }
            if ( Entry.Unit < Units.size() and Units[Entry.Unit] != nullptr )
            {
                const auto &Ast = *Units[Entry.Unit];
                if ( const auto *Decl = std::get_if<Frontend::Method>( &Ast.Decl( Entry.Decl ) ) )
                {
                    const InlineSummary Summary = SummarizeMethod( Ast, *Decl, Store, Id );
                    Entry.InlineVerdict = VerdictOf( Summary, Decl->bAbstract, Decl->bExternal or Entry.ExternSymbol.IsValid() );
                }
            }
        }

        std::unordered_map<Symbol, EInlineVerdict> VerdictByName;
        for ( const TypeSystem::Member &Entry : Members )
        {
            if ( Entry.Kind != TypeSystem::EMemberKind::Method )
            {
                continue;
            }
            auto It = VerdictByName.find( Entry.Name );
            if ( It == VerdictByName.end() )
            {
                VerdictByName.emplace( Entry.Name, Entry.InlineVerdict );
            }
            else
            {
                if ( Entry.InlineVerdict == EInlineVerdict::Never or It->second == EInlineVerdict::Never )
                {
                    It->second = EInlineVerdict::Never;
                }
                else if ( Entry.InlineVerdict == EInlineVerdict::Hint or It->second == EInlineVerdict::Hint )
                {
                    It->second = EInlineVerdict::Hint;
                }
            }
        }

        for ( TypeSystem::Member &Entry : Members )
        {
            if ( Entry.Kind == TypeSystem::EMemberKind::Method )
            {
                if ( auto It = VerdictByName.find( Entry.Name ); It != VerdictByName.end() )
                {
                    Entry.InlineVerdict = It->second;
                }
            }
        }
    }

    auto FreeFns = Store.MutableFreeFunctions();
    for ( TypeSystem::Member &Entry : FreeFns )
    {
        if ( Entry.Kind != TypeSystem::EMemberKind::Method )
        {
            continue;
        }
        if ( Entry.bAbstract or Entry.ExternSymbol.IsValid() )
        {
            Entry.InlineVerdict = EInlineVerdict::Never;
            continue;
        }
        if ( Entry.Unit < Units.size() and Units[Entry.Unit] != nullptr )
        {
            const auto &Ast = *Units[Entry.Unit];
            if ( const auto *Decl = std::get_if<Frontend::Method>( &Ast.Decl( Entry.Decl ) ) )
            {
                const InlineSummary Summary = SummarizeMethod( Ast, *Decl, Store, TypeSystem::NominalId{} );
                Entry.InlineVerdict = VerdictOf( Summary, Decl->bAbstract, Decl->bExternal or Entry.ExternSymbol.IsValid() );
            }
        }
    }

    std::unordered_map<Symbol, EInlineVerdict> FreeVerdictByName;
    for ( const TypeSystem::Member &Entry : FreeFns )
    {
        if ( Entry.Kind != TypeSystem::EMemberKind::Method )
        {
            continue;
        }
        auto It = FreeVerdictByName.find( Entry.Name );
        if ( It == FreeVerdictByName.end() )
        {
            FreeVerdictByName.emplace( Entry.Name, Entry.InlineVerdict );
        }
        else
        {
            if ( Entry.InlineVerdict == EInlineVerdict::Never or It->second == EInlineVerdict::Never )
            {
                It->second = EInlineVerdict::Never;
            }
            else if ( Entry.InlineVerdict == EInlineVerdict::Hint or It->second == EInlineVerdict::Hint )
            {
                It->second = EInlineVerdict::Hint;
            }
        }
    }

    for ( TypeSystem::Member &Entry : FreeFns )
    {
        if ( Entry.Kind == TypeSystem::EMemberKind::Method )
        {
            if ( auto It = FreeVerdictByName.find( Entry.Name ); It != FreeVerdictByName.end() )
            {
                Entry.InlineVerdict = It->second;
            }
        }
    }
}

} // namespace Volt::MiddleEnd::Optimisations
