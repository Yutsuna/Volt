#pragma once

#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/AST/Type.hpp"

#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace Volt::Frontend
{

inline ExprId CloneExpr ( const AstContext &Src, AstContext &Dst, ExprId Id );
inline StmtId CloneStmt ( const AstContext &Src, AstContext &Dst, StmtId Id );
inline DeclId CloneDecl ( const AstContext &Src, AstContext &Dst, DeclId Id );
inline TypeId CloneType ( const AstContext &Src, AstContext &Dst, TypeId Id );
inline ParamId CloneParam ( const AstContext &Src, AstContext &Dst, ParamId Id );

namespace Detail
{
    template <typename T> void CloneField ( const AstContext &Src, AstContext &Dst, T &Field )
    {
        using FieldType = std::remove_cvref_t<T>;
        if constexpr ( std::same_as<FieldType, ExprId> )
        {
            Field = CloneExpr( Src, Dst, Field );
        }
        else if constexpr ( std::same_as<FieldType, ExprList> or std::same_as<FieldType, Core::SmallVec<ExprId, 2>> or
                            std::same_as<FieldType, Core::SmallVec<ExprId, 4>> )
        {
            for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
            {
                Field[Index] = CloneExpr( Src, Dst, Field[Index] );
            }
        }
        else if constexpr ( std::same_as<FieldType, StmtId> )
        {
            Field = CloneStmt( Src, Dst, Field );
        }
        else if constexpr ( std::same_as<FieldType, StmtList> or std::same_as<FieldType, Core::SmallVec<StmtId, 2>> or
                            std::same_as<FieldType, Core::SmallVec<StmtId, 4>> )
        {
            for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
            {
                Field[Index] = CloneStmt( Src, Dst, Field[Index] );
            }
        }
        else if constexpr ( std::same_as<FieldType, DeclId> )
        {
            Field = CloneDecl( Src, Dst, Field );
        }
        else if constexpr ( std::same_as<FieldType, DeclList> or std::same_as<FieldType, Core::SmallVec<DeclId, 2>> or
                            std::same_as<FieldType, Core::SmallVec<DeclId, 4>> )
        {
            for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
            {
                Field[Index] = CloneDecl( Src, Dst, Field[Index] );
            }
        }
        else if constexpr ( std::same_as<FieldType, TypeId> )
        {
            Field = CloneType( Src, Dst, Field );
        }
        else if constexpr ( std::same_as<FieldType, TypeList> or std::same_as<FieldType, Core::SmallVec<TypeId, 2>> or
                            std::same_as<FieldType, Core::SmallVec<TypeId, 4>> )
        {
            for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
            {
                Field[Index] = CloneType( Src, Dst, Field[Index] );
            }
        }
        else if constexpr ( std::same_as<FieldType, ParamId> )
        {
            Field = CloneParam( Src, Dst, Field );
        }
        else if constexpr ( std::same_as<FieldType, ParamList> or std::same_as<FieldType, Core::SmallVec<ParamId, 2>> or
                            std::same_as<FieldType, Core::SmallVec<ParamId, 4>> )
        {
            for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
            {
                Field[Index] = CloneParam( Src, Dst, Field[Index] );
            }
        }
        else if constexpr ( std::same_as<FieldType, Symbol> )
        {
            Field = Dst.Strings().Intern( Src.Text( Field ) );
        }
        else if constexpr ( std::same_as<FieldType, SymbolList> or std::same_as<FieldType, Core::SmallVec<Symbol, 2>> or
                            std::same_as<FieldType, Core::SmallVec<Symbol, 4>> )
        {
            for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
            {
                Field[Index] = Dst.Strings().Intern( Src.Text( Field[Index] ) );
            }
        }
    }
} // namespace Detail

/// Deep-clone an expression subtree from `Src` AstContext into `Dst` AstContext.
/// Symbols are re-interned into Dst's StringInterner.
inline ExprId CloneExpr ( const AstContext &Src, AstContext &Dst, ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return ExprId{};
    }

    const ExprNode &SrcNode = Src.Expr( Id );

    const auto CloneAlt = [&] ( const auto &Alt ) -> ExprNode
    {
        using AltType = std::remove_cvref_t<decltype( Alt )>;
        AltType Copy  = Alt;
        Meta::ForEachField( Copy, [&] ( std::string_view, auto &Field ) { Detail::CloneField( Src, Dst, Field ); } );
        return ExprNode{ std::move( Copy ) };
    };

    ExprNode Cloned = std::visit( CloneAlt, SrcNode );
    return Dst.Add( std::move( Cloned ) );
}

inline StmtId CloneStmt ( const AstContext &Src, AstContext &Dst, StmtId Id )
{
    if ( not Id.IsValid() )
    {
        return StmtId{};
    }

    const StmtNode &SrcNode = Src.Stmt( Id );

    const auto CloneAlt = [&] ( const auto &Alt ) -> StmtNode
    {
        using AltType = std::remove_cvref_t<decltype( Alt )>;
        AltType Copy  = Alt;
        Meta::ForEachField( Copy, [&] ( std::string_view, auto &Field ) { Detail::CloneField( Src, Dst, Field ); } );
        return StmtNode{ std::move( Copy ) };
    };

    StmtNode Cloned = std::visit( CloneAlt, SrcNode );
    return Dst.Add( std::move( Cloned ) );
}

inline StmtList CloneStmtList ( const AstContext &Src, AstContext &Dst, const StmtList &List )
{
    StmtList Out;
    Out.Reserve( List.Size() );
    for ( const StmtId Stmt : List )
    {
        Out.PushBack( CloneStmt( Src, Dst, Stmt ) );
    }
    return Out;
}

inline DeclId CloneDecl ( const AstContext &Src, AstContext &Dst, DeclId Id )
{
    if ( not Id.IsValid() )
    {
        return DeclId{};
    }

    const DeclNode &SrcNode = Src.Decl( Id );

    const auto CloneAlt = [&] ( const auto &Alt ) -> DeclNode
    {
        using AltType = std::remove_cvref_t<decltype( Alt )>;
        AltType Copy  = Alt;
        Meta::ForEachField( Copy, [&] ( std::string_view, auto &Field ) { Detail::CloneField( Src, Dst, Field ); } );
        return DeclNode{ std::move( Copy ) };
    };

    DeclNode Cloned = std::visit( CloneAlt, SrcNode );
    return Dst.Add( std::move( Cloned ) );
}

inline TypeId CloneType ( const AstContext &Src, AstContext &Dst, TypeId Id )
{
    if ( not Id.IsValid() )
    {
        return TypeId{};
    }

    const TypeNode &SrcNode = Src.Type( Id );

    const auto CloneAlt = [&] ( const auto &Alt ) -> TypeNode
    {
        using AltType = std::remove_cvref_t<decltype( Alt )>;
        AltType Copy  = Alt;
        Meta::ForEachField( Copy, [&] ( std::string_view, auto &Field ) { Detail::CloneField( Src, Dst, Field ); } );
        return TypeNode{ std::move( Copy ) };
    };

    TypeNode Cloned = std::visit( CloneAlt, SrcNode );
    return Dst.Add( std::move( Cloned ) );
}

inline ParamId CloneParam ( const AstContext &Src, AstContext &Dst, ParamId Id )
{
    if ( not Id.IsValid() )
    {
        return ParamId{};
    }

    const Param &SrcNode = Src.GetParam( Id );
    Param Copy           = SrcNode;
    Meta::ForEachField( Copy, [&] ( std::string_view, auto &Field ) { Detail::CloneField( Src, Dst, Field ); } );
    return Dst.Add( std::move( Copy ) );
}

} // namespace Volt::Frontend
