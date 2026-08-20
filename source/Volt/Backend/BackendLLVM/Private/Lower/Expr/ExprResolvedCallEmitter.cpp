// ExprResolvedCallEmitter.cpp — the one call site every resolved callee goes
// through, and the machine conversions that have no body to call.
//
// Everything about a call that is *not* syntax is decided here from what Sema
// already recorded: whether it is indirect, whether it constructs, which
// instantiation it names, which parameter each argument fills, and whether the
// callee can raise. Nothing is re-derived — CalleeEntry is read, never
// recomputed.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Functions/FunctionRegistry.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/Exception/ExceptionLowering.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Lower/Mono/MonoDriver.hpp"
#include "Types/TypeMapper.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

llvm::Value *Volt::Backend::Llvm::BodyEmitter::EmitResolvedCall ( Frontend::ExprId Id,
                                                                  const MiddleEnd::IR::CalleeEntry &Entry,
                                                                  Frontend::ExprId Receiver,
                                                                  std::span<const Frontend::ExprId> Args,
                                                                  Frontend::ExprId Block )
{
    EmitterServices &Svc = Services();

    // An indirect callee has no body and no symbol: the callable being invoked
    // is the receiver, and the call goes through its `{ code, env }` pair rather
    // than to a mangled name. Sema decided this (CalleeEntry::bIndirect);
    // nothing is re-derived here.
    if ( Entry.bIndirect )
    {
        return EmitIndirectDispatch( *this, Id, Entry, Receiver, Args, Block );
    }

    const MiddleEnd::TypeSystem::UnitTypes &Values = *Frame().Values;

    // A machine conversion tagged by MemberResolver on a bodyless
    // non-operator member of a Pointer/Primitive receiver. The backend reads
    // the enum — no Volt name ever enters this module.
    if ( Entry.Decl->bAbstract )
    {
        using MC = MiddleEnd::IR::EMachineConversion;
        switch ( Entry.MachineConversion )
        {
        case MC::PtrToInt:
        {
            llvm::Value *Self = EmitExpr( Receiver );
            return Self ? Ctx().Builder().CreatePtrToInt( Self, Ctx().Builder().getInt64Ty() ) : nullptr;
        }
        case MC::IntToPtr:
        {
            llvm::Value *Addr = Args.empty() ? nullptr : EmitExpr( Args[0] );
            return Addr ? Ctx().Builder().CreateIntToPtr( Addr, llvm::PointerType::get( Ctx().Context(), 0 ) ) : nullptr;
        }
        case MC::None:
            break;
        }
        static_cast<void>( Fail( "llvm: call at expression " + std::to_string( Id.Value ) +
                                 " resolves to an abstract member with no body, and neither a machine conversion "
                                 "nor a builtin operator is registered for its receiver's layout" ) );
        return nullptr;
    }

    // The owner and the instantiation both come out of the entry Sema recorded:
    // NominalIds are the cross-unit currency, and the flattened bindings are the
    // same encoding the mangler and the layout cache key on.
    MiddleEnd::TypeSystem::NominalId Owner;
    if ( Values.Has( Entry.Receiver ) )
    {
        Owner = Values.Get( Entry.Receiver ).Base;
    }

    // A member `Owner` does not itself own (found on some ancestor or mixin
    // instead — LookupMember's own doc: "the MemberRef it hands back carries the
    // declaring nominal, not an instantiation") has no body defined against
    // *this* receiver: DefineAll only ever defines a member under the NominalId
    // that owns it in `Store.Type( Id ).Members`, never under every type that
    // merely inherits it. A mixin's own default (`Arithmetic#min`) is
    // additionally never defined at all — IsMixinOwner excludes it from both
    // sweeps, since its `self` means "whichever type includes me" and has no
    // signature until one does. This is exactly what a generic body is:
    // unresolved until a call site fixes it. So an inherited default is routed
    // through the same Monomorphizer queue below, keyed on the *receiver's*
    // Owner+Name — MangleFunction already mangles the receiver, not the
    // declaring mixin, so `Int32.min`/`Float64.min` land on distinct symbols
    // even though both share one AST body.
    bool bOwnMember = false;
    if ( Owner.IsValid() )
    {
        for ( const MiddleEnd::TypeSystem::Member &Candidate : Svc.Build->Types->Type( Owner ).Members )
        {
            if ( &Candidate == Entry.Decl )
            {
                bOwnMember = true;
                break;
            }
        }
    }
    const bool bInherited = Owner.IsValid() and not bOwnMember;

    // `Entry.Bindings` is the *declaring* type's parameter space, then the
    // method's own generics. For an inherited default that declaring type is the
    // mixin, whose space is empty — so taking it verbatim would erase the
    // receiver's arguments, and `Pointer<UInt8>#!=` would instantiate with no `T`
    // at all: its body's `self <=> other` would then resolve on a `Pointer` with
    // no argument, mangle to a symbol nothing defines, and the failure would
    // surface as an undefined symbol at link. The request is keyed on the
    // receiver (MangleFunction mangles the receiver, not the declaring mixin), so
    // the receiver's own arguments are what must reach it; the method's own
    // generic slots still come from the resolution.
    std::vector<std::uint32_t> FlatArgs;
    if ( bInherited and Values.Has( Entry.Receiver ) and not Values.Get( Entry.Receiver ).Args.IsEmpty() )
    {
        for ( const MiddleEnd::TypeSystem::SemaTypeId Arg : Values.Get( Entry.Receiver ).Args )
        {
            Types().FlattenValueType( Values, Arg, FlatArgs );
        }
        const std::size_t Own   = std::min<std::size_t>( Entry.Decl->OwnGenerics, Entry.Bindings.Size() );
        const std::size_t First = Entry.Bindings.Size() - Own;
        for ( std::size_t Index = First; Index < Entry.Bindings.Size(); ++Index )
        {
            Types().FlattenValueType( Values, Entry.Bindings[Index], FlatArgs );
        }
    }
    else
    {
        for ( const MiddleEnd::TypeSystem::SemaTypeId Binding : Entry.Bindings )
        {
            Types().FlattenValueType( Values, Binding, FlatArgs );
        }
    }

    // A generic owner or a method with its own generics has no body yet —
    // DeclareAll/DefineAll both skip it outright, since neither sweep knows the
    // arguments this call site just fixed. FunctionFor below still synthesises
    // its *declaration* on demand (so a forward or mutually recursive call
    // resolves), but the body is Monomorphizer's job: enqueue is idempotent
    // (MonoRequest::Key dedupes), so every call site reaching the same
    // instantiation is free to ask again. An inherited default is enqueued
    // unconditionally on FlatArgs — the receiver alone (`Owner`) is enough to key
    // the request even when neither the receiver nor the method itself is
    // generic.
    const bool bGenericOwner = Owner.IsValid() and Svc.Build->Types->Type( Owner ).Params.Size() > 0;
    if ( ( ( bGenericOwner or Entry.Decl->OwnGenerics > 0 ) and not FlatArgs.empty() ) or bInherited )
    {
        Svc.Mono->Enqueue( MonoRequest{ .Owner = Owner, .Name = Entry.Decl->Name, .Args = FlatArgs } );
    }

    llvm::Function *Callee = Svc.Functions->FunctionFor( *Entry.Decl, Owner, FlatArgs );
    if ( Callee == nullptr )
    {
        return nullptr;
    }

    std::vector<llvm::Value *> Actuals;
    Actuals.reserve( Args.size() + 1 );

    // abi.md's order, and the same one the declare sweep built the signature
    // with: `self` first, then the declared parameters.
    const bool bExternal = Entry.Decl->ExternSymbol.IsValid();

    // `T.new( … )` (Sema's CalleeEntry::bConstructs): the receiver expression
    // names a *type*, so there is nothing to evaluate for it — the storage the
    // initializer writes into has to come from this call, and that storage is
    // what the whole expression evaluates to, not the initializer's own result.
    llvm::Value *Constructed = nullptr;
    if ( Owner.IsValid() and not Entry.Decl->bSelf and not bExternal )
    {
        llvm::Value *Self = nullptr;
        if ( Entry.bConstructs )
        {
            const MiddleEnd::TypeSystem::LayoutId Shape = Types().LayoutOfValue( Values, Entry.Result );
            llvm::Type *Instance                        = Types().TypeOfLayout( Shape );
            if ( Instance == nullptr or not IsAggregate( Shape ) )
            {
                // A non-aggregate `self` is passed by value (abi.md), so an
                // initializer could not write through it. Nothing in the stdlib
                // constructs one; saying so beats a silent no-op.
                static_cast<void>( Fail( "llvm: the value constructed at expression " + std::to_string( Id.Value ) +
                                         " has no aggregate layout to initialise" ) );
                return nullptr;
            }
            Constructed = MakeTemp( Instance, "new" );
            Self        = Constructed;
        }
        else if ( Receiver.IsValid() )
        {
            Self = EmitExpr( Receiver );
        }
        else if ( Frame().Self != nullptr )
        {
            // A bare `capture_backtrace()` inside a method body: Sema resolved
            // the name on `self` (ExprInferencer's Identifier case, "inside a
            // method body a bare name is a member of self"), so the callee is an
            // Identifier with no object to evaluate and the receiver is this
            // frame's own.
            Self = Frame().Self;
        }
        else
        {
            static_cast<void>( Fail( "llvm: instance call at expression " + std::to_string( Id.Value ) +
                                     " (kind: " + std::string( Frontend::NodeName( Frame().Unit->Ast->Expr( Id ) ) ) +
                                     ", callee: '" + std::string( Svc.Build->Types->Text( Entry.Decl->Name ) ) + "') in unit " +
                                     std::string( Frame().Unit->Path ) + " has no receiver expression" ) );
            return nullptr;
        }

        if ( Self == nullptr )
        {
            return nullptr;
        }
        Actuals.push_back( Self );
    }

    // The declared parameters, in their own order — which is not the argument
    // list's: a `&block` slot binds through the call's trailing `do ... end` and
    // is skipped by positional matching (TypeStore::Member), so the two are
    // walked together rather than one being assumed to be the other.
    std::size_t Positional = 0;
    bool bBlockBound       = false;
    for ( std::size_t Index = 0; Index < Entry.Decl->Params.Size(); ++Index )
    {
        const bool bBlockSlot = Index < Entry.Decl->ParamIsBlock.Size() and Entry.Decl->ParamIsBlock[Index];

        Frontend::ExprId Arg;
        if ( bBlockSlot )
        {
            Arg         = Block;
            bBlockBound = true;
        }
        else if ( Positional < Args.size() )
        {
            Arg = Args[Positional];
            ++Positional;
        }

        if ( not Arg.IsValid() )
        {
            if ( not Failed() )
            {
                static_cast<void>( Fail( "llvm: the call at expression " + std::to_string( Id.Value ) +
                                         " supplies no argument for parameter " + std::to_string( Index ) + " of '" +
                                         std::string( Svc.Build->Types->Text( Entry.Decl->Name ) ) + "'" ) );
            }
            return nullptr;
        }

        llvm::Value *Value = EmitExpr( Arg );
        if ( Value == nullptr )
        {
            return nullptr;
        }
        Actuals.push_back( Value );
    }

    if ( Block.IsValid() and not bBlockBound )
    {
        static_cast<void>( Fail( "llvm: the call at expression " + std::to_string( Id.Value ) +
                                 " carries a trailing block, and '" + std::string( Svc.Build->Types->Text( Entry.Decl->Name ) ) +
                                 "' declares no `&block` parameter to bind it to" ) );
        return nullptr;
    }

    llvm::FunctionType *Signature = Callee->getFunctionType();
    if ( Actuals.size() != Signature->getNumParams() )
    {
        static_cast<void>( Fail( "llvm: call at expression " + std::to_string( Id.Value ) + " passes " +
                                 std::to_string( Actuals.size() ) + " arguments to a signature taking " +
                                 std::to_string( Signature->getNumParams() ) ) );
        return nullptr;
    }
    for ( std::size_t Index = 0; Index < Actuals.size(); ++Index )
    {
        Actuals[Index] = CoerceWidth( Actuals[Index], Signature->getParamType( static_cast<unsigned>( Index ) ) );
    }
    llvm::Value *Result = Ctx().Builder().CreateCall( Callee, Actuals );

    // The mirror of CoerceWidth's aggregate case: a struct comes back by value,
    // and every consumer here expects an aggregate to *be* an address. Spilling
    // it into this frame's own slot is also what makes the result outlive the
    // callee's storage, which is the whole reason it was returned by value
    // rather than as a pointer.
    if ( Result->getType()->isStructTy() )
    {
        llvm::Value *Slot = MakeTemp( Result->getType(), "call.result" );
        if ( Slot == nullptr )
        {
            static_cast<void>(
                Fail( "llvm: no frame to hold the aggregate result of the call at expression " + std::to_string( Id.Value ) ) );
            return nullptr;
        }
        static_cast<void>( Ctx().Builder().CreateStore( Result, Slot ) );
        Result = Slot;
    }

    // An `@[External]` symbol in non-volt libraries is C, and C cannot raise a
    // Volt exception — the thread-local state it might happen to read was some
    // *other* Volt call's, never its own, so the post-call check only runs for
    // Volt code (or external symbols marked with library "volt", such as
    // _V_init_all).
    const bool bVoltUnwindable =
        not bExternal or ( Entry.Decl->ExternLib.IsValid() and Svc.Build != nullptr and Svc.Build->Types != nullptr and
                           Svc.Build->Types->Text( Entry.Decl->ExternLib ) == "volt" );
    if ( bVoltUnwindable )
    {
        if ( bBlockBound )
        {
            // This call is the one the trailing `do...end` was bound to — the
            // exact scope a `break` inside it terminates (backend phase 6c).
            // Consumed here, not propagated: an exception is a different signal
            // and still takes the ordinary poisoned path.
            Svc.Exceptions->EmitExceptionCheck( *this );
            if ( not Terminated() )
            {
                static_cast<void>( Ctx().Builder().CreateStore( Ctx().Builder().getFalse(), Svc.Exceptions->BreakFlagSlot() ) );
            }
        }
        else
        {
            Svc.Exceptions->EmitUnwindCheck( *this );
        }
    }
    return Constructed != nullptr ? Constructed : Result;
}
