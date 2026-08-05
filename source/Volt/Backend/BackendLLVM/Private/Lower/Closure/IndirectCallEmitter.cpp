// IndirectCallEmitter.cpp — invoking a callable *value*.
//
// `f( x )` on a local holding a closure and `block.call( x )` on a `&block`
// parameter are the same emission: the callee has no symbol, so LLVM needs the
// signature spelled out from the receiver's own type arguments — the one place
// in this module where a FunctionType is built from a *type* rather than from a
// declaration, because a callable has no declaration to build it from.

#include "Lower/Closure/ClosureLowering.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/Exception/ExceptionLowering.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

llvm::Value *Volt::Backend::Llvm::ClosureLowering::EmitIndirectCall ( BodyEmitter &Emitter,
                                                                      Frontend::ExprId Id,
                                                                      const Sema::CalleeEntry &Entry,
                                                                      Frontend::ExprId Receiver,
                                                                      std::span<const Frontend::ExprId> Args )
{
    if ( not Receiver.IsValid() )
    {
        static_cast<void>( Emitter.Fail( "llvm: the callable invoked at expression " + std::to_string( Id.Value ) +
                                         " has no receiver expression" ) );
        return nullptr;
    }

    const Sema::UnitTypes &Values = *Emitter.Frame().Values;
    llvm::LLVMContext &Context    = Services->Ctx->Context();
    llvm::IRBuilder<> &Builder    = Services->Ctx->Builder();
    TypeMapper &Types             = Emitter.Types();

    // The signature *is* the receiver's own type arguments, and MemberResolver
    // already unpacked them into the entry: result first, then one parameter per
    // remaining argument. So the shape of this call comes from the type of the
    // thing being called — nothing is re-derived.
    std::vector<llvm::Type *> Slots;
    Slots.reserve( Entry.Params.Size() + 1 );
    for ( const Sema::SemaTypeId Param : Entry.Params )
    {
        llvm::Type *Slot = Types.ParamTypeOfLayout( Types.LayoutOfValue( Values, Param ) );
        if ( Slot == nullptr )
        {
            static_cast<void>( Emitter.Fail( "llvm: the callable invoked at expression " + std::to_string( Id.Value ) +
                                             " has a parameter with no resolved layout" ) );
            return nullptr;
        }
        Slots.push_back( Slot );
    }
    Slots.push_back( llvm::PointerType::get( Context, 0 ) );

    llvm::Type *Result = Types.TypeOfLayout( Types.LayoutOfValue( Values, Entry.Result ) );
    if ( Result == nullptr )
    {
        Result = llvm::Type::getVoidTy( Context );
    }

    if ( Args.size() != Entry.Params.Size() )
    {
        static_cast<void>( Emitter.Fail( "llvm: the callable invoked at expression " + std::to_string( Id.Value ) +
                                         " is passed " + std::to_string( Args.size() ) +
                                         " arguments against a type carrying " + std::to_string( Entry.Params.Size() ) ) );
        return nullptr;
    }

    // The receiver may be the very node carrying this call's own `bIndirect`
    // CalleeEntry — `f( x )` on a closure-typed local resolves with
    // `Receiver == Node.Callee`, the `f` Identifier itself. `EmitExpr`'s
    // `Identifier`/`Member` visitors also consult `Frame.Callees->Get(Id)` (to
    // support a paren-less bare call, e.g. `test_int8`), so routing through them
    // here would re-discover this same entry and reinterpret "read the value of
    // `f`" as "invoke `f` again", recursing into this function with an invalid
    // receiver. `LoadPlace` reads the place directly — the only reading that
    // cannot rediscover a call.
    const Frontend::AstContext &Ast = *Emitter.Frame().Unit->Ast;
    const bool bAmbiguousReceiver   = std::holds_alternative<Frontend::Identifier>( Ast.Expr( Receiver ) ) or
                                    std::holds_alternative<Frontend::Member>( Ast.Expr( Receiver ) );
    llvm::Value *Pair = bAmbiguousReceiver ? Emitter.LoadPlace( Receiver ) : Emitter.EmitExpr( Receiver );
    if ( Pair == nullptr )
    {
        return nullptr;
    }

    llvm::StructType *Shape  = ClosurePairType();
    llvm::Type *Address      = llvm::PointerType::get( Context, 0 );
    llvm::Value *Code        = Builder.CreateLoad( Address, Builder.CreateStructGEP( Shape, Pair, 0, "callee.code" ), "code" );
    llvm::Value *Environment = Builder.CreateLoad( Address, Builder.CreateStructGEP( Shape, Pair, 1, "callee.env" ), "env" );

    llvm::FunctionType *Signature = llvm::FunctionType::get( Result, Slots, false );
    std::vector<llvm::Value *> Actuals;
    Actuals.reserve( Args.size() + 1 );
    for ( std::size_t Index = 0; Index < Args.size(); ++Index )
    {
        llvm::Value *Value = Emitter.EmitExpr( Args[Index] );
        if ( Value == nullptr )
        {
            return nullptr;
        }
        Actuals.push_back( Emitter.CoerceWidth( Value, Signature->getParamType( static_cast<unsigned>( Index ) ) ) );
    }
    Actuals.push_back( Environment );

    llvm::Value *Call = Builder.CreateCall( Signature, Code, Actuals );

    // The mirror of EmitResolvedCall's own aggregate case: a struct comes back by
    // value, and every consumer downstream expects an aggregate to *be* an
    // address — a curried closure returning another closure
    // (`f = (x) => (y) => x + y; g = f(20)`) is the case that made this visible,
    // since Entry.Result is itself the `{code,env}` aggregate. Without this spill
    // the raw SSA struct reached `Assign`'s `EmitStore`, which memcpys from an
    // address — an ill-formed `llvm.memcpy` operand, rejected by the module
    // verifier.
    if ( Call->getType()->isStructTy() )
    {
        llvm::Value *Slot = Emitter.MakeTemp( Call->getType(), "call.result" );
        if ( Slot == nullptr )
        {
            static_cast<void>( Emitter.Fail(
                "llvm: no frame to hold the aggregate result of the indirect call at expression " +
                std::to_string( Id.Value ) ) );
            return nullptr;
        }
        static_cast<void>( Builder.CreateStore( Call, Slot ) );
        Call = Slot;
    }

    // Always Volt code behind a callable — never external — so the post-call
    // check always runs, same as an ordinary resolved call. This is
    // `block.call(...)` itself: it must let a `break` inside the block keep
    // unwinding through it, never consume it — EmitUnwindCheck, not the
    // exception-only check.
    Services->Exceptions->EmitUnwindCheck( Emitter );
    return Call;
}
