#include "MemberResolver.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/MiddleEnd/TypeSystem/Instantiate.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"

#include "Volt/MiddleEnd/TypeSystem/TypeCompat.hpp"

#include "ExprInferencer.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeResolve.hpp"

#include <algorithm>

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

// `..`/`...` are spelled like operators (neither opens with a letter or
// `_`) but no instruction table row backs either of them — a range is an
// ordinary aggregate (`Range<T>`), resolved through `Comparable#..` like any
// other non-primitive operator. Excluding them here is what lets `1..3` fall
// through to real member lookup instead of being silently waved through as
// "the backend supplies this".
[[nodiscard]] bool IsRangeOperator ( std::string_view Name )
{
    return Name == ".." or Name == "...";
}

// Is `Name` an operator the machine itself provides on this receiver — that
// is, does the receiver collapse to a Primitive or a Pointer? Deliberately
// narrower than IsBuiltinOpOn, which also widens `===` to an enum: an enum is
// an aggregate, and no instruction table answers for one.
[[nodiscard]] bool IsMachineOperatorOn ( const Volt::MiddleEnd::Analysis::TypeCheckerContext &Context,
                                         Volt::MiddleEnd::TypeSystem::NominalId Base,
                                         std::string_view Name )
{
    using namespace Volt::MiddleEnd::TypeSystem;

    if ( not Base.IsValid() or not IsOperatorName( Name ) or IsRangeOperator( Name ) )
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

} // namespace

Volt::MiddleEnd::Analysis::Resolution
Volt::MiddleEnd::Analysis::LookupOn ( TypeCheckerContext &Context, SemaTypeId Receiver, std::string_view Name )
{
    if ( not Context.Ctx.Values.Has( Receiver ) )
    {
        return Resolution{};
    }

    const std::string_view CleanName = Name.starts_with( '@' ) ? Name.substr( 1 ) : Name;
    auto Found                       = LookupMemberOn( Context.Ctx.Types, Context.Ctx.Values, Receiver, Receiver, CleanName );

    if ( Found.Decl == nullptr and Context.Ctx.Values.Has( Receiver ) )
    {
        const NominalId Nominal = Context.Ctx.Values.Get( Receiver ).Base;
        if ( Nominal.IsValid() )
        {
            const std::string_view NominalName = Context.Ctx.Types.Text( Context.Ctx.Types.Type( Nominal ).Name );
            if ( NominalName == "IntLiteral" or NominalName == "FloatLiteral" )
            {
                if ( const auto FallbackBase = Context.Ctx.Types.LookupNodeKind( NominalName ) )
                {
                    const SemaTypeId FallbackType = Context.MakeType( *FallbackBase, {} );
                    Found = LookupMemberOn( Context.Ctx.Types, Context.Ctx.Values, FallbackType, FallbackType, CleanName );
                }
            }
        }
    }

    // `T.new` runs `initialize` but does not *evaluate* to what it returns:
    // a constructor yields the thing constructed. Its declared result is the
    // Void every initializer writes, so the receiver has to be substituted
    // back in, or `Array<U>.new` would be untyped.
    bool bConstructor         = false;
    bool bDynamicCall         = false;
    std::uint32_t DynamicSlot = 0;
    if ( Found.Decl == nullptr and CleanName == ConstructorCall )
    {
        Found        = LookupMemberOn( Context.Ctx.Types, Context.Ctx.Values, Receiver, Receiver, ConstructorName );
        bConstructor = Found.Decl != nullptr;
    }
    if ( Found.Decl == nullptr and Context.Ctx.Values.Has( Receiver ) and IsDynamicType( Context, Receiver ) )
    {
        const auto &ReceiverVal = Context.Ctx.Values.Get( Receiver );
        if ( not ReceiverVal.Args.IsEmpty() and ReceiverVal.Args[0].IsValid() )
        {
            const SemaTypeId TraitType = ReceiverVal.Args[0];
            Found                      = LookupMemberOn( Context.Ctx.Types, Context.Ctx.Values, TraitType, TraitType, CleanName );
            if ( Found.Decl != nullptr )
            {
                const NominalId TraitNominal = Context.Ctx.Values.Get( TraitType ).Base;
                DynamicSlot                  = Context.Ctx.Types.VTableSlotOf( TraitNominal, Found.Decl->Name );
                bDynamicCall                 = true;
            }
        }
    }
    if ( Found.Decl == nullptr )
    {
        return Resolution{};
    }

    // The type that actually *declares* what was found, dropping the
    // arguments LookupMemberOn made concrete. Two unrelated questions want
    // it: whether a mixin's default body is shadowing a machine operator
    // (below), and whether the caller may see the member at all
    // (CheckMemberAccess).
    const NominalId DeclaringBase =
        Context.Ctx.Values.Has( Found.Owner ) ? Context.Ctx.Values.Get( Found.Owner ).Base : NominalId{};

    // Invoking a callable. The signature is not what the contract wrote down
    // — it *cannot* be, since a Proc's arity lives in its type arguments —
    // so it is read off the receiver: result first, then one parameter per
    // remaining argument. Nothing here names a type or a member; the receiver
    // being the FuncType claimant is the whole test.
    if ( IsCallableType( Context, Receiver ) and Found.Decl->bAbstract )
    {
        const Volt::Core::SmallVec<SemaTypeId, 2> &Signature = Context.Ctx.Values.Get( Receiver ).Args;
        Volt::Core::SmallVec<SemaTypeId, 4> Params;
        for ( std::size_t Index = 1; Index < Signature.Size(); ++Index )
        {
            Params.PushBack( Signature[Index] );
        }
        // No block slot: the signature is the receiver's own arguments, and a
        // callable takes its arguments positionally.
        return Resolution{ .Decl       = Found.Decl,
                           .Owner      = DeclaringBase,
                           .Result     = Signature.IsEmpty() ? SemaTypeId{} : Signature[0],
                           .Params     = std::move( Params ),
                           .BlockParam = SemaTypeId{},
                           .Bindings   = {},
                           .Receiver   = Receiver,
                           .bIndirect  = true };
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
    Resolution Out{ .Decl             = Found.Decl,
                    .Owner            = DeclaringBase,
                    .Result           = SemaTypeId{},
                    .Params           = {},
                    .BlockParam       = SemaTypeId{},
                    .Bindings         = Context.Ctx.Values.Get( Found.Owner ).Args,
                    .Receiver         = Receiver,
                    .bIndirect        = bDynamicCall,
                    .VTableSlot       = DynamicSlot,
                    .bDynamicDispatch = bDynamicCall };
    for ( std::uint32_t Slot = 0; Slot < Found.Decl->OwnGenerics; ++Slot )
    {
        Out.Bindings.PushBack( SemaTypeId{} );
    }

    Reinstantiate( Context, Out );
    if ( bConstructor )
    {
        Out.Result      = Receiver;
        Out.bConstructs = true;
    }

    // On a Primitive or Pointer layout an operator *is* a machine instruction
    // (rules/zero-hardcode.md), so a mixin's default body must not shadow one:
    // `Comparable#<` is written as `( self <=> other ) < 0`, and its own `<`
    // resolves straight back to itself — a call would recurse until the stack
    // ends. Dropping only the declaration keeps the signature that gave the
    // expression its type (and gave the right operand its expected type), and
    // "no Decl on a primitive receiver" is exactly what tells a backend to
    // select an instruction (rules/core-ast.md).
    //
    // A declaration on the receiver's *own* nominal still wins, because it is
    // the only thing that can express what the machine has no opcode for:
    // `Int32#<=>` and `Pointer<T>#<=>` are real bodies, written with those
    // very instructions.
    const NominalId ReceiverBase = Context.Ctx.Values.Get( Receiver ).Base;
    if ( DeclaringBase != ReceiverBase and IsMachineOperatorOn( Context, ReceiverBase, CleanName ) )
    {
        Out.Decl = nullptr;
    }
    return Out;
}

Volt::MiddleEnd::Analysis::Resolution Volt::MiddleEnd::Analysis::LookupFreeFunction ( TypeCheckerContext &Context,
                                                                                      std::string_view Name )
{
    const Member *Found = Context.Ctx.Types.LookupFunction( Name );
    if ( Found == nullptr )
    {
        return Resolution{};
    }

    // No Owner: a top-level `def` is declared by no type, so there is nothing
    // for a visibility check to be relative to and nothing that could hide it.
    Resolution Out{ .Decl       = Found,
                    .Owner      = NominalId{},
                    .Result     = SemaTypeId{},
                    .Params     = {},
                    .BlockParam = SemaTypeId{},
                    .Bindings   = {},
                    .Receiver   = SemaTypeId{} };
    for ( std::uint32_t Slot = 0; Slot < Found->OwnGenerics; ++Slot )
    {
        Out.Bindings.PushBack( SemaTypeId{} );
    }

    Reinstantiate( Context, Out );
    return Out;
}

void Volt::MiddleEnd::Analysis::Reinstantiate ( TypeCheckerContext &Context, Resolution &Found )
{
    // An indirect callee's signature came from its receiver's type arguments,
    // not from `Decl->Params`; recomputing it from the declaration would
    // overwrite it with the empty contract. For dynamic dispatch, however,
    // the signature comes from the trait's method declaration.
    if ( Found.Decl == nullptr or ( Found.bIndirect and not Found.bDynamicDispatch ) )
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

    if ( Found.Decl->Result.IsValid() )
    {
        Found.Result = Instantiate( Context.Ctx.Types, Found.Decl->Result, Applied, Found.Receiver, Context.Ctx.Values );
    }
    else if ( not Found.bConstructs and not Found.Owner.IsValid() and Found.Decl->Kind == EMemberKind::Method and
              not Found.Decl->bAbstract and not Context.bGenericBody )
    {
        bool bAllBindingsConcrete = true;
        for ( const SemaTypeId Binding : Found.Bindings )
        {
            if ( not Binding.IsValid() )
            {
                bAllBindingsConcrete = false;
                break;
            }
        }
        if ( Found.Decl->OwnGenerics > 0 and Found.Bindings.IsEmpty() )
        {
            bAllBindingsConcrete = false;
        }
        if ( bAllBindingsConcrete and Found.Decl->Decl.IsValid() )
        {
            const Frontend::AstContext *DeclAst =
                ( Found.Decl->Unit < Context.Ctx.AllUnits.size() and Context.Ctx.AllUnits[Found.Decl->Unit] != nullptr )
                    ? Context.Ctx.AllUnits[Found.Decl->Unit]
                    : &Context.Ctx.Ast;
            if ( DeclAst != nullptr and Found.Decl->Decl.Value < DeclAst->DeclCount() )
            {
                if ( const auto *MethodNode = std::get_if<Frontend::Method>( &DeclAst->Decl( Found.Decl->Decl ) );
                     MethodNode != nullptr and not MethodNode->ReturnType.IsValid() )
                {
                    std::vector<std::uint32_t> FlatArgs;
                    if ( Found.Receiver.IsValid() and Context.Ctx.Values.Has( Found.Receiver ) )
                    {
                        for ( const SemaTypeId Arg : Context.Ctx.Values.Get( Found.Receiver ).Args )
                        {
                            TypeSystem::FlattenValueType( Context.Ctx.Values, Arg, FlatArgs );
                        }
                    }
                    for ( const SemaTypeId Binding : Found.Bindings )
                    {
                        TypeSystem::FlattenValueType( Context.Ctx.Values, Binding, FlatArgs );
                    }
                    const SemaTypeId Ret = TypeSystem::InferMethodReturnType(
                        Context.Ctx.Types, *DeclAst, Context.Ctx.Scopes, *Found.Decl, Found.Owner, FlatArgs, Context.Ctx.Values );
                    if ( Ret.IsValid() )
                    {
                        Found.Result = Ret;
                    }
                }
            }
        }
    }
}

void Volt::MiddleEnd::Analysis::UnifyArgs ( TypeCheckerContext &Context, Resolution &Found, const Frontend::ExprList &Args )
{
    if ( Found.Decl == nullptr )
    {
        return;
    }

    // `OwnGenerics == 0` usually means "nothing new to learn": the ordinary
    // case is a receiver whose own generic arguments are already concrete
    // (`arr.push(x)` on a bound `Array<Int32>`), and inside a generic
    // definition's own body an unbound `Binding` is *expected* and must stay
    // that way until instantiation (`rules/core-ast.md`'s "generic
    // definition bodies" contract) — proceeding there would unify against
    // something not ready yet. Scoped narrowly to `EnumCase`, the one kind
    // that is never itself generic (`TypeBinder.cpp`'s Phase B never sets
    // `OwnGenerics` on it) yet can still carry an unbound *owner* slot: a
    // naked-type receiver used as a constructor (`Optional::Some(
    // "Yutsuna" )`, `Optional` with no `<T>` written) seeds `Bindings` with
    // invalid placeholders (`PlaceholderTypeArgs`, `ExprInferencer.cpp`)
    // precisely so this call can still teach it what `T` is.
    if ( Found.Decl->OwnGenerics == 0 and Found.Decl->Kind != EMemberKind::EnumCase )
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

void Volt::MiddleEnd::Analysis::UnifyBlock ( TypeCheckerContext &Context, Resolution &Found, SemaTypeId BlockType )
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

void Volt::MiddleEnd::Analysis::CheckMemberSelf ( TypeCheckerContext &Context,
                                                  Volt::Core::SourceRange Loc,
                                                  const Resolution &Found,
                                                  bool bReceiverIsNakedType )
{
    if ( Found.Decl == nullptr or Found.Decl->Kind == EMemberKind::EnumCase )
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

void Volt::MiddleEnd::Analysis::CheckMemberAccess ( TypeCheckerContext &Context,
                                                    Volt::Core::SourceRange Loc,
                                                    const Resolution &Found )
{
    using Frontend::EVisibility;

    if ( Found.Decl == nullptr or not Found.Owner.IsValid() )
    {
        return;
    }

    const EVisibility Written = Found.Decl->Visibility;
    if ( Written == EVisibility::None or Written == EVisibility::Public )
    {
        return;
    }

    // `private` reaches exactly one type; `protected` reaches it and its
    // descendants. Both are answered against the body we are inside, so a
    // reference outside every type body (`SelfType` invalid, as at file
    // scope) fails both — which is the point.
    const bool bVisible = Written == EVisibility::Private ? Context.SelfType == Found.Owner
                                                          : ConformsTo( Context.Ctx.Types, Context.SelfType, Found.Owner );
    if ( bVisible )
    {
        return;
    }

    const std::string_view Keyword = Written == EVisibility::Private ? "private" : "protected";
    Context.Report( Loc, std::string{ Keyword } + " member " + std::string{ Context.Ctx.Types.Text( Found.Decl->Name ) } +
                             " of " + Context.NameOf( Found.Owner ) + " cannot be accessed here" );
}

bool Volt::MiddleEnd::Analysis::IsCallableType ( const TypeCheckerContext &Context, SemaTypeId Receiver )
{
    if ( not Context.Ctx.Values.Has( Receiver ) )
    {
        return false;
    }
    const NominalId Base = Context.Ctx.Values.Get( Receiver ).Base;
    if ( not Base.IsValid() )
    {
        return false;
    }
    const auto Callable = Context.Ctx.Types.LookupNodeKind( "FuncType" );
    return Callable.has_value() and *Callable == Base;
}

bool Volt::MiddleEnd::Analysis::IsDynamicType ( const TypeCheckerContext &Context, SemaTypeId Receiver )
{
    if ( not Context.Ctx.Values.Has( Receiver ) )
    {
        return false;
    }
    const NominalId Base = Context.Ctx.Values.Get( Receiver ).Base;
    if ( not Base.IsValid() )
    {
        return false;
    }
    const auto DynamicKind = Context.Ctx.Types.LookupNodeKind( "DynamicType" );
    return DynamicKind.has_value() and *DynamicKind == Base;
}

Volt::Frontend::ExprId
Volt::MiddleEnd::Analysis::CoerceToDynamic ( TypeCheckerContext &Context, Volt::Frontend::ExprId ValExpr, SemaTypeId TargetType )
{
    if ( not ValExpr.IsValid() or not Context.Ctx.Values.Has( TargetType ) )
    {
        return ValExpr;
    }
    const SemaTypeId ValType = Context.Ctx.Values.ExprType( ValExpr );
    if ( not Context.Ctx.Values.Has( ValType ) )
    {
        return ValExpr;
    }
    const auto &TargetVal = Context.Ctx.Values.Get( TargetType );
    const auto &ValVal    = Context.Ctx.Values.Get( ValType );
    if ( Context.Ctx.Types.IsNodeKind( "DynamicType", TargetVal.Base ) and
         not Context.Ctx.Types.IsNodeKind( "DynamicType", ValVal.Base ) and not TargetVal.Args.IsEmpty() and
         TargetVal.Args[0].IsValid() )
    {
        const Frontend::ExprId UpcastId = Context.Ctx.Ast.Add( Frontend::ExprNode{ Frontend::DynamicUpcast{
            .Loc         = Frontend::LocOf( Context.Ctx.Ast.Expr( ValExpr ) ),
            .Value       = ValExpr,
            .TargetTrait = Frontend::TypeId{},
        } } );
        Context.Ctx.Values.SetExprType( UpcastId, TargetType );
        return UpcastId;
    }
    return ValExpr;
}

Volt::MiddleEnd::Analysis::Resolution Volt::MiddleEnd::Analysis::LookupCallOn ( TypeCheckerContext &Context, SemaTypeId Receiver )
{
    if ( not IsCallableType( Context, Receiver ) )
    {
        return Resolution{};
    }

    // The contract, not a spelling: the callable type declares exactly one
    // abstract member and that member is what invoking a value means. Reading
    // it out of the store rather than writing "call" here is what keeps the
    // member's name Volt's to choose.
    const NominalId Base = Context.Ctx.Values.Get( Receiver ).Base;
    for ( const Member &Entry : Context.Ctx.Types.Type( Base ).Members )
    {
        if ( Entry.Kind == EMemberKind::Method and Entry.bAbstract )
        {
            // Re-resolved by name rather than used directly, so the call goes
            // through the same instantiation path as `f.call( x )` would.
            return LookupOn( Context, Receiver, Context.Ctx.Types.Text( Entry.Name ) );
        }
    }
    return Resolution{};
}

bool Volt::MiddleEnd::Analysis::HasEnumCases ( const TypeStore &Store, NominalId Nominal )
{
    if ( not Nominal.IsValid() )
    {
        return false;
    }
    const auto &Members = Store.Type( Nominal ).Members;
    return std::ranges::any_of( Members, [] ( const Member &Entry ) { return Entry.Kind == EMemberKind::EnumCase; } );
}

bool Volt::MiddleEnd::Analysis::IsBuiltinOpOn ( const TypeCheckerContext &Context, NominalId Base, std::string_view Name )
{
    if ( not Base.IsValid() or not IsOperatorName( Name ) or IsRangeOperator( Name ) )
    {
        return false;
    }

    const NominalType &Nominal = Context.Ctx.Types.Type( Base );
    if ( not Nominal.Layout.IsValid() )
    {
        return false;
    }

    const LayoutKind Kind = KindOf( Context.Ctx.Types.Get( Nominal.Layout ) );
    if ( Kind == LayoutKind::Primitive or Kind == LayoutKind::Pointer )
    {
        return true;
    }

    // An enum's layout is an empty `Aggregate` (no `@[Primitive]`, no
    // fields) — never `Primitive`/`Pointer` — yet `when Enum::Case` desugars
    // to `pattern === target` (CaseLowering) and no enum declares `===`
    // itself. `"==="` is the only operator this exemption widens to, since
    // that is the only one CaseLowering ever emits against an enum.
    return Name == "===" and HasEnumCases( Context.Ctx.Types, Base );
}

bool Volt::MiddleEnd::Analysis::IsMachineSuppliedOn ( const TypeCheckerContext &Context,
                                                      Volt::MiddleEnd::TypeSystem::NominalId Base )
{
    using namespace Volt::MiddleEnd::TypeSystem;

    if ( not Base.IsValid() )
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

Volt::MiddleEnd::TypeSystem::SemaTypeId Volt::MiddleEnd::Analysis::MemberType (
    TypeCheckerContext &Context, Frontend::ExprId Id, SemaTypeId Receiver, bool bReceiverIsNakedType, std::string_view Name )
{
    const Volt::Core::SourceRange Loc = Frontend::LocOf( Context.Ctx.Ast.Expr( Id ) );
    const Resolution Found            = LookupOn( Context, Receiver, Name );

    // Unconditional, exactly as the Member branch has always done: an empty
    // Resolution is itself the answer "no user-written member here", which for
    // an operator on a primitive layout is what tells a backend to emit an
    // instruction rather than a call.
    Context.CalleeResolution[Id.Value] = Found;

    if ( Context.Ctx.Values.Has( Receiver ) and Found.Decl == nullptr )
    {
        const NominalId Base = Context.Ctx.Values.Get( Receiver ).Base;
        if ( not IsBuiltinOpOn( Context, Base, Name ) )
        {
            Context.Report( Loc, "type " + Context.NameOfValue( Receiver ) + " has no member '" + std::string( Name ) + "'" );
        }
    }
    else if ( Found.Decl != nullptr and Found.Decl->bAbstract and not Found.bIndirect )
    {
        const NominalId ReceiverBase = Context.Ctx.Values.Has( Receiver ) ? Context.Ctx.Values.Get( Receiver ).Base : NominalId{};
        if ( not IsMachineSuppliedOn( Context, ReceiverBase ) and not Context.Ctx.Types.IsMixin( Context.SelfType ) )
        {
            Context.Report( Loc, "cannot call abstract method '" + std::string( Name ) + "' on type " +
                                     Context.NameOfValue( Receiver ) + " without dynamic dispatch" );
            Context.CalleeResolution.erase( Id.Value );
        }
        else if ( IsMachineSuppliedOn( Context, ReceiverBase ) and not IsOperatorName( Name ) )
        {
            // A bodyless non-operator member on a Pointer/Primitive receiver:
            // the backend supplies a machine conversion, tagged by signature
            // shape so no Volt name ever enters the backend.
            using MC    = Volt::MiddleEnd::IR::EMachineConversion;
            auto &Entry = Context.CalleeResolution[Id.Value];
            if ( Found.Decl->bSelf and Found.Decl->Params.Size() == 1 )
            {
                Entry.MachineConversion = MC::IntToPtr;
            }
            else if ( not Found.Decl->bSelf and Found.Decl->Params.Size() == 0 )
            {
                Entry.MachineConversion = MC::PtrToInt;
            }
        }
    }
    CheckMemberSelf( Context, Loc, Found, bReceiverIsNakedType );
    CheckMemberAccess( Context, Loc, Found );
    return Found.Result;
}

void Volt::MiddleEnd::Analysis::CheckCallArgs ( TypeCheckerContext &Context,
                                                Volt::Core::SourceRange Loc,
                                                const Resolution &Found,
                                                const Frontend::ExprList &Args )
{
    if ( Found.Decl == nullptr or Found.Decl->Kind != EMemberKind::Method )
    {
        return;
    }

    const std::string Name = std::string{ Context.Ctx.Types.Text( Found.Decl->Name ) };

    const std::size_t Expected    = Found.Params.Size();
    const std::size_t MinExpected = Found.Decl->MinParams;
    const std::size_t Given       = Args.Size();
    if ( Given < MinExpected or Given > Expected )
    {
        Context.Report( Loc, Name + " takes " +
                                 ( MinExpected == Expected ? std::to_string( Expected )
                                                           : std::to_string( MinExpected ) + ".." + std::to_string( Expected ) ) +
                                 " argument(s), but " + std::to_string( Given ) + " were given" );
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
        if ( ArgType.IsValid() and ParamType.IsValid() and not IsAssignable( Context, ParamType, ArgType ) )
        {
            Context.Report( Frontend::LocOf( Context.Ctx.Ast.Expr( Args[Index] ) ),
                            "argument " + std::to_string( Index + 1 ) + " to " + Name + " has type " +
                                Context.NameOfValue( ArgType ) + ", expected " + Context.NameOfValue( ParamType ) );
        }
    }
}

void Volt::MiddleEnd::Analysis::CheckArity ( TypeCheckerContext &Context,
                                             Volt::Core::SourceRange Loc,
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
