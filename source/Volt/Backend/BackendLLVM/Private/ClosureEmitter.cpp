// ClosureEmitter.cpp — the indirect call through a closure value, and the
// `next`-as-`ret` half of the non-local-exit protocol a lifted closure body
// shares with its call site.
//   - `EmitIndirectCall` is how a callable *value* (a local, a parameter, a
//     `FuncAddr`-carrying `Proc`) gets invoked — the callee has no symbol, so
//     LLVM needs the signature spelled out from the receiver's own type;
//   - `EmitBlockNext` is `next`'s meaning inside a synthesized function with
//     no loop of its own (`Frame.bClosure`, set by `DefineSynthesizedFn`):
//     it ends the invocation, i.e. `ret`, not a branch.

#include "LlvmState.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>

#include <cstddef>
#include <string>
#include <vector>

llvm::StructType *Volt::Backend::Llvm::LlvmBackend::State::ClosurePairType ()
{
    llvm::Type *Address = llvm::PointerType::get( Context, 0 );
    // A literal struct type, uniqued by LLVM on its element types — so this is
    // the same `{ ptr, ptr }` TypeMapper builds from the layout InstanceLayouts
    // materialises for a callable, not a second shape that happens to match.
    return llvm::StructType::get( Context, { Address, Address }, false );
}

void Volt::Backend::Llvm::LlvmBackend::State::EmitBlockNext ( Frontend::ExprId Value )
{
    if ( not Frame.bReturnsValue )
    {
        if ( Value.IsValid() )
        {
            static_cast<void>( Fail( "llvm: `next` carries a value out of a closure whose type declares no result" ) );
            return;
        }
        static_cast<void>( Builder->CreateRetVoid() );
        return;
    }

    if ( not Value.IsValid() )
    {
        // A bare `next` in a value-producing block: the same hole a body
        // falling off its end leaves, and lowered the same way.
        static_cast<void>( Builder->CreateUnreachable() );
        return;
    }

    if ( llvm::Value *Result = EmitExpr( Value ); Result != nullptr )
    {
        static_cast<void>( Builder->CreateRet( CoerceWidth( Result, Frame.Fn->getReturnType() ) ) );
    }
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitIndirectCall ( Frontend::ExprId Id,
                                                                         const Sema::CalleeEntry &Entry,
                                                                         Frontend::ExprId Receiver,
                                                                         std::span<const Frontend::ExprId> Args )
{
    if ( not Receiver.IsValid() )
    {
        static_cast<void>(
            Fail( "llvm: the callable invoked at expression " + std::to_string( Id.Value ) + " has no receiver expression" ) );
        return nullptr;
    }

    const Sema::UnitTypes &Values = *Frame.Values;

    // The signature *is* the receiver's own type arguments, and MemberResolver
    // already unpacked them into the entry: result first, then one parameter
    // per remaining argument. So the shape of this call comes from the type of
    // the thing being called — nothing is re-derived.
    std::vector<llvm::Type *> Slots;
    Slots.reserve( Entry.Params.Size() + 1 );
    for ( const Sema::SemaTypeId Param : Entry.Params )
    {
        llvm::Type *Slot = ParamTypeOfLayout( LayoutOfValue( Values, Param ) );
        if ( Slot == nullptr )
        {
            static_cast<void>( Fail( "llvm: the callable invoked at expression " + std::to_string( Id.Value ) +
                                     " has a parameter with no resolved layout" ) );
            return nullptr;
        }
        Slots.push_back( Slot );
    }
    Slots.push_back( llvm::PointerType::get( Context, 0 ) );

    llvm::Type *Result = TypeOfLayout( LayoutOfValue( Values, Entry.Result ) );
    if ( Result == nullptr )
    {
        Result = llvm::Type::getVoidTy( Context );
    }

    if ( Args.size() != Entry.Params.Size() )
    {
        static_cast<void>( Fail( "llvm: the callable invoked at expression " + std::to_string( Id.Value ) + " is passed " +
                                 std::to_string( Args.size() ) + " arguments against a type carrying " +
                                 std::to_string( Entry.Params.Size() ) ) );
        return nullptr;
    }

    // The receiver may be the very node carrying this call's own `bIndirect`
    // CalleeEntry — `f( x )` on a closure-typed local resolves with
    // `Receiver == Node.Callee`, the `f` Identifier itself. `EmitExpr`'s
    // `Identifier`/`Member` visitors also consult `Frame.Callees->Get(Id)`
    // (to support a paren-less bare call, e.g. `test_int8`), so routing
    // through them here would re-discover this same entry and reinterpret
    // "read the value of `f`" as "invoke `f` again", recursing into this
    // function with an invalid receiver. `LoadPlace` reads the place
    // directly — the only reading that cannot rediscover a call.
    const bool bAmbiguousReceiver = std::holds_alternative<Frontend::Identifier>( Frame.Unit->Ast->Expr( Receiver ) ) or
                                    std::holds_alternative<Frontend::Member>( Frame.Unit->Ast->Expr( Receiver ) );
    llvm::Value *Pair = bAmbiguousReceiver ? LoadPlace( Receiver ) : EmitExpr( Receiver );
    if ( Pair == nullptr )
    {
        return nullptr;
    }

    llvm::StructType *Shape  = ClosurePairType();
    llvm::Type *Address      = llvm::PointerType::get( Context, 0 );
    llvm::Value *Code        = Builder->CreateLoad( Address, Builder->CreateStructGEP( Shape, Pair, 0, "callee.code" ), "code" );
    llvm::Value *Environment = Builder->CreateLoad( Address, Builder->CreateStructGEP( Shape, Pair, 1, "callee.env" ), "env" );

    llvm::FunctionType *Signature = llvm::FunctionType::get( Result, Slots, false );
    std::vector<llvm::Value *> Actuals;
    Actuals.reserve( Args.size() + 1 );
    for ( std::size_t Index = 0; Index < Args.size(); ++Index )
    {
        llvm::Value *Value = EmitExpr( Args[Index] );
        if ( Value == nullptr )
        {
            return nullptr;
        }
        Actuals.push_back( CoerceWidth( Value, Signature->getParamType( static_cast<unsigned>( Index ) ) ) );
    }
    Actuals.push_back( Environment );

    // An indirect call: the callee is a value, so LLVM needs the signature
    // spelled out — the one place in this emitter where a FunctionType is built
    // from a *type* rather than from a declaration, because a callable has no
    // declaration to build it from.
    llvm::Value *Call = Builder->CreateCall( Signature, Code, Actuals );

    // The mirror of EmitResolvedCall's own aggregate case (ExprEmitter.cpp):
    // a struct comes back by value, and every consumer downstream expects an
    // aggregate to *be* an address — a curried closure returning another
    // closure (`f = (x) => (y) => x + y; g = f(20)`) is the case that made
    // this visible, since Entry.Result is itself the `{code,env}` aggregate.
    // Without this spill the raw SSA struct reached `Assign`'s `EmitStore`,
    // which memcpys from an address — an ill-formed `llvm.memcpy` operand,
    // rejected by the module verifier.
    if ( Call->getType()->isStructTy() )
    {
        llvm::Value *Slot = MakeTemp( Call->getType(), "call.result" );
        if ( Slot == nullptr )
        {
            static_cast<void>( Fail( "llvm: no frame to hold the aggregate result of the indirect call at expression " +
                                     std::to_string( Id.Value ) ) );
            return nullptr;
        }
        static_cast<void>( Builder->CreateStore( Call, Slot ) );
        Call = Slot;
    }

    // Always Volt code behind a callable — never external — so the
    // post-call check always runs, same as an ordinary resolved call. This is
    // `block.call(...)` itself: it must let a `break` inside the block keep
    // unwinding through it, never consume it — EmitUnwindCheck, not the
    // exception-only check.
    EmitUnwindCheck();
    return Call;
}
