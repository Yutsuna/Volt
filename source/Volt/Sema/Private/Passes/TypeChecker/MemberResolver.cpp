#include "MemberResolver.hpp"

#include "ExprInferencer.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

Volt::Sema::TypeCheckerPass::Resolution
Volt::Sema::TypeCheckerPass::LookupOn ( TypeCheckerContext &Context, SemaTypeId Receiver, std::string_view Name )
{
    if ( not Context.Ctx.Values.Has( Receiver ) )
    {
        return Resolution{};
    }

    const std::string_view CleanName = Name.starts_with( '@' ) ? Name.substr( 1 ) : Name;
    auto Found                       = LookupMemberOn( Context.Ctx.Types, Context.Ctx.Values, Receiver, Receiver, CleanName );

    // `T.new` runs `initialize` but does not *evaluate* to what it returns:
    // a constructor yields the thing constructed. Its declared result is the
    // Void every initializer writes, so the receiver has to be substituted
    // back in, or `Array<U>.new` would be untyped.
    bool bConstructor = false;
    if ( Found.Decl == nullptr and CleanName == ConstructorCall )
    {
        Found        = LookupMemberOn( Context.Ctx.Types, Context.Ctx.Values, Receiver, Receiver, ConstructorName );
        bConstructor = Found.Decl != nullptr;
    }
    if ( Found.Decl == nullptr )
    {
        return Resolution{};
    }

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

    // A signature's ParamIndex counts against the type that *declares* it,
    // and LookupMemberOn already made that owner concrete: `each` reached
    // from `Array<Int32>` is owned by `Enumerable<Int32>`, so its parameter
    // 0 is Int32. `Self` stays the receiver — a mixin's `self` means the
    // type that included it, never the mixin.
    const Core::SmallVec<SemaTypeId, 2> OwnerArgs = Context.Ctx.Values.Get( Found.Owner ).Args;
    const std::span<const SemaTypeId> Applied{ OwnerArgs.begin(), OwnerArgs.Size() };

    Core::SmallVec<SemaTypeId, 4> Params;
    for ( std::size_t Index = 0; Index < Found.Decl->Params.Size(); ++Index )
    {
        if ( Index < Found.Decl->ParamIsBlock.Size() and Found.Decl->ParamIsBlock[Index] )
        {
            continue;
        }
        Params.PushBack( Instantiate( Context.Ctx.Types, Found.Decl->Params[Index], Applied, Receiver, Context.Ctx.Values ) );
    }

    const SemaTypeId Result =
        bConstructor ? Receiver : Instantiate( Context.Ctx.Types, Found.Decl->Result, Applied, Receiver, Context.Ctx.Values );

    return Resolution{ .Decl = Found.Decl, .Result = Result, .Params = std::move( Params ) };
}

void Volt::Sema::TypeCheckerPass::CheckMemberSelf ( TypeCheckerContext &Context,
                                                    Core::SourceRange Loc,
                                                    const Resolution &Found,
                                                    bool bReceiverIsNakedType )
{
    if ( Found.Decl == nullptr )
    {
        return;
    }

    const bool bMemberIsStatic = Found.Decl->bSelf;
    const std::string Name     = std::string{ Context.Ctx.Types.Text( Found.Decl->Name ) };

    if ( bReceiverIsNakedType and not bMemberIsStatic and Name != ConstructorName )
    {
        Context.Report( Loc, "instance member " + Name + " cannot be accessed on a type" );
    }
    else if ( not bReceiverIsNakedType and bMemberIsStatic )
    {
        Context.Report( Loc, "static member " + Name + " cannot be accessed on an instance" );
    }
}

void Volt::Sema::TypeCheckerPass::CheckDotCallSelf ( TypeCheckerContext &Context, Core::SourceRange Loc, const Resolution &Found )
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

bool Volt::Sema::TypeCheckerPass::IsBuiltinPrimitiveOp ( std::string_view Name )
{
    if ( Name.empty() )
    {
        return false;
    }
    if ( Name == "and" or Name == "or" or Name == "not" )
    {
        return true;
    }
    const char Ch = Name[0];
    return ( Ch < 'a' or Ch > 'z' ) and ( Ch < 'A' or Ch > 'Z' ) and Ch != '_';
}

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::MemberType (
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

void Volt::Sema::TypeCheckerPass::CheckCallArgs ( TypeCheckerContext &Context,
                                                  Core::SourceRange Loc,
                                                  const Resolution &Found,
                                                  const Frontend::ExprList &Args )
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

void Volt::Sema::TypeCheckerPass::CheckArity ( TypeCheckerContext &Context,
                                               Core::SourceRange Loc,
                                               NominalId Base,
                                               std::size_t Given )
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
