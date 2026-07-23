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
        // No block slot: an `@[Apply]` signature is the receiver's own
        // arguments, and a callable takes its argument positionally.
        return Resolution{ .Decl       = Found.Decl,
                           .Result     = Signature.IsEmpty() ? SemaTypeId{} : Signature[0],
                           .Params     = std::move( Params ),
                           .BlockParam = SemaTypeId{},
                           .Bindings   = {},
                           .Receiver   = Receiver };
    }

    // A signature's ParamIndex counts against the type that *declares* it,
    // and LookupMemberOn already made that owner concrete: `each` reached
    // from `Array<Int32>` is owned by `Enumerable<Int32>`, so its parameter
    // 0 is Int32. `Self` stays the receiver — a mixin's `self` means the
    // type that included it, never the mixin.
    //
    // After those come the method's own generics, as holes: `def map<U>`
    // adds one slot nothing here can fill. Inference at the call site
    // closes them and recomputes, which is what Reinstantiate is for.
    Resolution Out{ .Decl       = Found.Decl,
                    .Result     = SemaTypeId{},
                    .Params     = {},
                    .BlockParam = SemaTypeId{},
                    .Bindings   = Context.Ctx.Values.Get( Found.Owner ).Args,
                    .Receiver   = Receiver };
    for ( std::uint32_t Slot = 0; Slot < Found.Decl->OwnGenerics; ++Slot )
    {
        Out.Bindings.PushBack( SemaTypeId{} );
    }

    Reinstantiate( Context, Out );
    if ( bConstructor )
    {
        Out.Result = Receiver;
    }
    return Out;
}

void Volt::Sema::TypeCheckerPass::Reinstantiate ( TypeCheckerContext &Context, Resolution &Found )
{
    if ( Found.Decl == nullptr or Found.Decl->bApply )
    {
        return;
    }

    const std::span<const SemaTypeId> Applied{ Found.Bindings.begin(), Found.Bindings.Size() };

    Found.Params.Clear();
    Found.BlockParam = SemaTypeId{};
    for ( std::size_t Index = 0; Index < Found.Decl->Params.Size(); ++Index )
    {
        const SemaTypeId Slot =
            Instantiate( Context.Ctx.Types, Found.Decl->Params[Index], Applied, Found.Receiver, Context.Ctx.Values );

        // A `&block` slot is not a positional parameter — it binds through
        // the call's trailing `do ... end`. Kept aside rather than dropped:
        // it is the only thing that can type that block's parameters.
        if ( Index < Found.Decl->ParamIsBlock.Size() and Found.Decl->ParamIsBlock[Index] )
        {
            Found.BlockParam = Slot;
            continue;
        }
        Found.Params.PushBack( Slot );
    }

    Found.Result = Instantiate( Context.Ctx.Types, Found.Decl->Result, Applied, Found.Receiver, Context.Ctx.Values );
}

void Volt::Sema::TypeCheckerPass::UnifyArgs ( TypeCheckerContext &Context, Resolution &Found, const Frontend::ExprList &Args )
{
    if ( Found.Decl == nullptr or Found.Decl->OwnGenerics == 0 )
    {
        return;
    }

    // Positional arguments against positional slots — the `&block` slot is
    // skipped here exactly as Reinstantiate skips it, so the two stay in
    // step and argument N keeps meaning the same parameter in both.
    const std::span<SemaTypeId> Bindings{ Found.Bindings.begin(), Found.Bindings.Size() };
    std::size_t Position = 0;
    for ( std::size_t Index = 0; Index < Found.Decl->Params.Size(); ++Index )
    {
        if ( Index < Found.Decl->ParamIsBlock.Size() and Found.Decl->ParamIsBlock[Index] )
        {
            continue;
        }
        if ( Position < Args.Size() )
        {
            UnifySig( Context.Ctx.Types, Context.Ctx.Values, Found.Decl->Params[Index],
                      Context.Ctx.Values.ExprType( Args[Position] ), Bindings );
        }
        ++Position;
    }

    Reinstantiate( Context, Found );
}

void Volt::Sema::TypeCheckerPass::UnifyBlock ( TypeCheckerContext &Context, Resolution &Found, SemaTypeId BlockType )
{
    if ( Found.Decl == nullptr or Found.Decl->OwnGenerics == 0 or not BlockType.IsValid() )
    {
        return;
    }

    // This is where `U` is learnt: the declared `&block : T -> U` matched
    // against the `Proc< Bool, Int32 >` the trailing block turned out to
    // be. It has to happen *after* the block is typed, which is why the
    // block is typed after the callee and before the result is read.
    const std::span<SemaTypeId> Bindings{ Found.Bindings.begin(), Found.Bindings.Size() };
    for ( std::size_t Index = 0; Index < Found.Decl->Params.Size(); ++Index )
    {
        if ( Index < Found.Decl->ParamIsBlock.Size() and Found.Decl->ParamIsBlock[Index] )
        {
            UnifySig( Context.Ctx.Types, Context.Ctx.Values, Found.Decl->Params[Index], BlockType, Bindings );
        }
    }

    Reinstantiate( Context, Found );
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

namespace
{

// An operator name, told apart from an ordinary member by spelling alone:
// anything that does not open with a letter or an underscore, plus the three
// word-spelled logical operators. No operator is listed here by hand.
[[nodiscard]] bool IsOperatorName ( std::string_view Name )
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

} // namespace

Volt::Sema::TypeCheckerPass::Resolution Volt::Sema::TypeCheckerPass::LookupApplyOn ( TypeCheckerContext &Context,
                                                                                     SemaTypeId Receiver )
{
    if ( not Context.Ctx.Values.Has( Receiver ) )
    {
        return Resolution{};
    }

    const NominalId Base = Context.Ctx.Values.Get( Receiver ).Base;
    if ( not Base.IsValid() )
    {
        return Resolution{};
    }

    for ( const Member &Entry : Context.Ctx.Types.Type( Base ).Members )
    {
        if ( Entry.bApply )
        {
            // Re-resolved by name rather than used directly, so the call goes
            // through the same instantiation path as `f.call( x )` would.
            return LookupOn( Context, Receiver, Context.Ctx.Types.Text( Entry.Name ) );
        }
    }
    return Resolution{};
}

bool Volt::Sema::TypeCheckerPass::IsBuiltinOpOn ( const TypeCheckerContext &Context, NominalId Base, std::string_view Name )
{
    if ( not Base.IsValid() or not IsOperatorName( Name ) )
    {
        return false;
    }

    const NominalType &Nominal = Context.Ctx.Types.Type( Base );
    if ( not Nominal.Layout.IsValid() )
    {
        return false;
    }

    const LayoutKind Kind = KindOf( Context.Ctx.Types.Get( Nominal.Layout ) );
    return Kind == LayoutKind::Primitive or Kind == LayoutKind::Pointer;
}

Volt::Sema::SemaTypeId Volt::Sema::TypeCheckerPass::MemberType (
    TypeCheckerContext &Context, Core::SourceRange Loc, SemaTypeId Receiver, bool bReceiverIsNakedType, std::string_view Name )
{
    const Resolution Found = LookupOn( Context, Receiver, Name );
    if ( Context.Ctx.Values.Has( Receiver ) and Found.Decl == nullptr )
    {
        const NominalId Base = Context.Ctx.Values.Get( Receiver ).Base;
        if ( not IsBuiltinOpOn( Context, Base, Name ) )
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
