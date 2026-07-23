// MemberResolver.cpp — Implementation of member resolution and call checking.

#include "MemberResolver.hpp"

#include "ExprInferencer.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

namespace Volt::Sema::TypeCheckerPass
{

Resolution LookupOn ( TypeCheckerContext &Context, SemaTypeId Receiver, std::string_view Name )
{
    if ( not Context.Ctx.Values.Has( Receiver ) )
    {
        return Resolution{};
    }

    const NominalId Base             = Context.Ctx.Values.Get( Receiver ).Base;
    const std::string_view CleanName = Name.starts_with( '@' ) ? Name.substr( 1 ) : Name;
    auto Found                       = Context.Ctx.Types.LookupMember( Base, CleanName );
    if ( Found.Decl == nullptr and CleanName == "new" )
    {
        Found = Context.Ctx.Types.LookupMember( Base, "initialize" );
    }
    if ( Found.Decl == nullptr )
    {
        return Resolution{};
    }

    Core::SmallVec<SemaTypeId, 2> Args;
    if ( Found.Owner == Base )
    {
        for ( const SemaTypeId Arg : Context.Ctx.Values.Get( Receiver ).Args )
        {
            Args.PushBack( Arg );
        }
    }

    const std::span<const SemaTypeId> Applied{ Args.begin(), Args.Size() };

    if ( Found.Decl->bApply )
    {
        const Core::SmallVec<SemaTypeId, 2> &Signature = Context.Ctx.Values.Get( Receiver ).Args;
        Core::SmallVec<SemaTypeId, 4> Params;
        for ( std::size_t Index = 1; Index < Signature.Size(); ++Index )
        {
            Params.PushBack( Signature[Index] );
        }
        return Resolution{
            .Decl = Found.Decl, .Result = Signature.IsEmpty() ? SemaTypeId{} : Signature[0], .Params = std::move( Params ) };
    }

    Core::SmallVec<SemaTypeId, 4> Params;
    for ( std::size_t Index = 0; Index < Found.Decl->Params.Size(); ++Index )
    {
        if ( Index < Found.Decl->ParamIsBlock.Size() and Found.Decl->ParamIsBlock[Index] )
        {
            continue;
        }
        Params.PushBack( Instantiate( Context.Ctx.Types, Found.Decl->Params[Index], Applied, Receiver, Context.Ctx.Values ) );
    }

    return Resolution{ .Decl   = Found.Decl,
                       .Result = Instantiate( Context.Ctx.Types, Found.Decl->Result, Applied, Receiver, Context.Ctx.Values ),
                       .Params = std::move( Params ) };
}

void CheckMemberSelf ( TypeCheckerContext &Context, Core::SourceRange Loc, const Resolution &Found, bool bReceiverIsNakedType )
{
    if ( Found.Decl == nullptr )
    {
        return;
    }

    const bool bMemberIsStatic = Found.Decl->bSelf;
    const std::string Name     = std::string{ Context.Ctx.Types.Text( Found.Decl->Name ) };

    if ( bReceiverIsNakedType and not bMemberIsStatic and Name != "initialize" )
    {
        Context.Report( Loc, "instance member " + Name + " cannot be accessed on a type" );
    }
    else if ( not bReceiverIsNakedType and bMemberIsStatic )
    {
        Context.Report( Loc, "static member " + Name + " cannot be accessed on an instance" );
    }
}

void CheckDotCallSelf ( TypeCheckerContext &Context, Core::SourceRange Loc, const Resolution &Found )
{
    if ( Found.Decl == nullptr )
    {
        return;
    }

    const bool bMemberIsStatic = Found.Decl->bSelf;
    const std::string Name     = std::string{ Context.Ctx.Types.Text( Found.Decl->Name ) };

    if ( Context.bStaticContext and not bMemberIsStatic )
    {
        Context.Report( Loc, "instance member " + Name + " cannot be accessed from a static context" );
    }
    else if ( not Context.bStaticContext and bMemberIsStatic )
    {
        Context.Report( Loc, "static member " + Name + " cannot be accessed from an instance context" );
    }
}

bool IsBuiltinPrimitiveOp ( std::string_view Name )
{
    if ( Name.empty() )
    {
        return false;
    }
    if ( Name == "and" or Name == "or" or Name == "not" )
    {
        return true;
    }
    const char c = Name[0];
    return not( ( c >= 'a' and c <= 'z' ) or ( c >= 'A' and c <= 'Z' ) or c == '_' );
}

SemaTypeId MemberType (
    TypeCheckerContext &Context, Core::SourceRange Loc, SemaTypeId Receiver, bool bReceiverIsNakedType, std::string_view Name )
{
    const Resolution Found = LookupOn( Context, Receiver, Name );
    if ( Context.Ctx.Values.Has( Receiver ) and Found.Decl == nullptr )
    {
        bool bIsBuiltinOp          = false;
        const NominalId Base       = Context.Ctx.Values.Get( Receiver ).Base;
        const NominalType &Nominal = Context.Ctx.Types.Type( Base );
        if ( Nominal.Layout.IsValid() )
        {
            const LayoutKind Kind = KindOf( Context.Ctx.Types.Get( Nominal.Layout ) );
            if ( ( Kind == LayoutKind::Primitive or Kind == LayoutKind::Pointer ) and IsBuiltinPrimitiveOp( Name ) )
            {
                bIsBuiltinOp = true;
            }
        }

        if ( not bIsBuiltinOp )
        {
            Context.Report( Loc, "type " + Context.NameOfValue( Receiver ) + " has no member '" + std::string( Name ) + "'" );
        }
    }
    CheckMemberSelf( Context, Loc, Found, bReceiverIsNakedType );
    return Found.Result;
}

void CheckCallArgs ( TypeCheckerContext &Context, Core::SourceRange Loc, const Resolution &Found, const Frontend::ExprList &Args )
{
    if ( Found.Decl == nullptr or Found.Decl->Kind != EMemberKind::Method )
    {
        return;
    }

    const std::string Name = std::string{ Context.Ctx.Types.Text( Found.Decl->Name ) };

    const std::size_t Expected = Found.Params.Size();
    const std::size_t Given    = Args.Size();
    if ( Given != Expected )
    {
        Context.Report( Loc, Name + " takes " + std::to_string( Expected ) + " argument(s), but " + std::to_string( Given ) +
                                 " were given" );
        return;
    }

    for ( std::size_t Index = 0; Index < Given; ++Index )
    {
        const SemaTypeId ParamType = Found.Params[Index];
        if ( ParamType.IsValid() )
        {
            Context.ConstrainExprType( Args[Index], ParamType );
        }
        const SemaTypeId ArgType = InferExpr( Context, Args[Index] );
        if ( ArgType.IsValid() and ParamType.IsValid() and ArgType.Value != ParamType.Value )
        {
            Context.Report( Frontend::LocOf( Context.Ctx.Ast.Expr( Args[Index] ) ),
                            "argument " + std::to_string( Index + 1 ) + " to " + Name + " has type " +
                                Context.NameOfValue( ArgType ) + ", expected " + Context.NameOfValue( ParamType ) );
        }
    }
}

void CheckArity ( TypeCheckerContext &Context, Core::SourceRange Loc, NominalId Base, std::size_t Given )
{
    if ( not Base.IsValid() )
    {
        return;
    }
    const std::size_t Expected = Context.Ctx.Types.Type( Base ).Params.Size();
    if ( Given != Expected )
    {
        Context.Report( Loc, Context.NameOf( Base ) + " takes " + std::to_string( Expected ) + " type argument(s), but " +
                                 std::to_string( Given ) + " were given" );
    }
}

} // namespace Volt::Sema::TypeCheckerPass
