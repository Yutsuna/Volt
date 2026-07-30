// ClosureEmitter.cpp — Lambda / Block, their environments, and the indirect
// call through a closure value.
//
// Nothing here decides what a closure captures, how big its environment is, or
// where a field of it lives: `SynthesizeClosureFrame` (Sema) already fixed
// `{ Fields[Offset], TotalSize, Alignment, bEscapes }`, and this only allocates
// that shape and fills it. Three conventions, all from .agents/backend/abi.md:
//
//   - the closure *value* is the two-slot `{ code, env }` aggregate, so — like
//     every aggregate — it is an address (`InstanceLayouts` materialises that
//     layout, which is why a local, a field and a parameter holding a callable
//     all agree on its shape);
//   - a closure body is a private function whose parameters are the declared
//     ones followed by `ptr %env`, the trailing position abi.md fixes for all
//     three targets;
//   - an env field holds the **address** of the captured binding, never a copy
//     of its value. That is what makes a capture indistinguishable from any
//     other slot inside the body — `FunctionFrame::Slots` maps a BindingSite to
//     an address everywhere else too — and it is the only reading the frame's
//     uniform pointer-sized fields support.

#include "LlvmState.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

llvm::StructType *Volt::Backend::Llvm::LlvmBackend::State::ClosurePairType ()
{
    llvm::Type *Address = llvm::PointerType::get( Context, 0 );
    // A literal struct type, uniqued by LLVM on its element types — so this is
    // the same `{ ptr, ptr }` TypeMapper builds from the layout InstanceLayouts
    // materialises for a callable, not a second shape that happens to match.
    return llvm::StructType::get( Context, { Address, Address }, false );
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitLambda ( Frontend::ExprId Id, const Frontend::Lambda &Node )
{
    return EmitClosure( Id, Node.Params, Node.Body, nullptr );
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitBlock ( Frontend::ExprId Id, const Frontend::Block &Node )
{
    return EmitClosure( Id, Node.Params, Frontend::ExprId{}, &Node.Body );
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitClosure ( Frontend::ExprId Id,
                                                                    const Frontend::ParamList &Params,
                                                                    Frontend::ExprId Body,
                                                                    const Frontend::StmtList *Stmts )
{
    const Sema::ScopeId Scope = Frame.Unit->Scopes->ScopeOfExpr( Id );
    if ( not Scope.IsValid() )
    {
        static_cast<void>( Fail( "llvm: closure at expression " + std::to_string( Id.Value ) +
                                 " has no scope — ScopeResolver records one for every closure literal" ) );
        return nullptr;
    }

    // The captures, their offsets and whether the environment escapes are all
    // read here and never recomputed: ScopeResolver proved the escape, and
    // SynthesizeClosureFrame laid the fields out.
    const Sema::ClosureEnvFrame Env = Sema::SynthesizeClosureFrame( *Frame.Unit->Scopes, *Frame.Values, Scope );

    llvm::Function *Code = EmitClosureBody( Id, Env, Params, Body, Stmts );
    if ( Code == nullptr )
    {
        return nullptr;
    }

    llvm::Value *Environment = EmitClosureEnv( Id, Env );
    if ( Environment == nullptr )
    {
        return nullptr;
    }

    llvm::StructType *Shape = ClosurePairType();
    llvm::Value *Pair       = MakeTemp( Shape, "closure" );
    if ( Pair == nullptr )
    {
        static_cast<void>( Fail( "llvm: no storage for the closure value at expression " + std::to_string( Id.Value ) ) );
        return nullptr;
    }

    static_cast<void>( Builder->CreateStore( Code, Builder->CreateStructGEP( Shape, Pair, 0, "closure.code" ) ) );
    static_cast<void>( Builder->CreateStore( Environment, Builder->CreateStructGEP( Shape, Pair, 1, "closure.env" ) ) );
    return Pair;
}

llvm::Function *Volt::Backend::Llvm::LlvmBackend::State::EmitClosureBody ( Frontend::ExprId Id,
                                                                           const Sema::ClosureEnvFrame &Env,
                                                                           const Frontend::ParamList &Params,
                                                                           Frontend::ExprId Body,
                                                                           const Frontend::StmtList *Stmts )
{
    const UnitView &Unit          = *Frame.Unit;
    const Sema::UnitTypes &Values = *Frame.Values;

    std::vector<llvm::Type *> Slots;
    Slots.reserve( Params.Size() + 1 );
    for ( const Frontend::ParamId ParamRef : Params )
    {
        // The parameter's type is the one TypeChecker wrote onto its binding
        // site — for `arr.each do | i |` that is the type the callee's `&block`
        // slot gave it, and it exists nowhere else.
        llvm::Type *Slot = ParamTypeOfLayout( LayoutOfValue( Values, Values.SiteType( Sema::BindingSite{ ParamRef } ) ) );
        if ( Slot == nullptr )
        {
            static_cast<void>( Fail( "llvm: closure parameter '" +
                                     std::string( Unit.Ast->Text( Unit.Ast->GetParam( ParamRef ).Name ) ) +
                                     "' has no resolved layout — an unannotated parameter with no expected type is a known "
                                     "middle-end gap (rules/core-ast.md)" ) );
            return nullptr;
        }
        Slots.push_back( Slot );
    }

    // `ptr %env` trails the declared parameters (abi.md). It is present even
    // when nothing is captured, so the call site needs no second signature: an
    // empty environment is a null pointer, not a missing argument.
    Slots.push_back( llvm::PointerType::get( Context, 0 ) );

    // The result is the callable type Sema gave the closure itself: a callable
    // carries its arity and its result in its *type* (`Proc`'s arguments are
    // the result followed by the parameters), so this reads what the middle-end
    // already concluded rather than re-deriving it from the body.
    llvm::Type *Result          = llvm::Type::getVoidTy( Context );
    const Sema::SemaTypeId Own  = Values.ExprType( Id );
    const bool bHasCallableType = Values.Has( Own ) and Values.Get( Own ).Args.Size() > 0;
    if ( bHasCallableType )
    {
        if ( llvm::Type *Declared = TypeOfLayout( LayoutOfValue( Values, Values.Get( Own ).Args[0] ) ); Declared != nullptr )
        {
            Result = Declared;
        }
    }

    llvm::FunctionType *Signature = llvm::FunctionType::get( Result, Slots, false );

    // Private, and named only for a human reading the IR: a closure is reached
    // through its `{ code, env }` pair, never by symbol, so it has no mangling
    // and takes no part in the declare sweep's symbol table.
    const std::string Name = Frame.Fn->getName().str() + ".closure" + std::to_string( ClosureCount++ );
    llvm::Function *Fn     = llvm::Function::Create( Signature, llvm::Function::PrivateLinkage, Name, Mod.get() );

    // A nested frame: the enclosing body's slots, loop stack and insert point
    // must survive the detour, and none of them means anything inside this one.
    FunctionFrame Enclosing                   = std::move( Frame );
    const llvm::IRBuilderBase::InsertPoint At = Builder->saveIP();

    Frame               = FunctionFrame{};
    Frame.Fn            = Fn;
    Frame.Unit          = Enclosing.Unit;
    Frame.Values        = Enclosing.Values;
    Frame.Callees       = Enclosing.Callees;
    Frame.Owner         = Enclosing.Owner;
    Frame.OwnerArgs     = Enclosing.OwnerArgs;
    Frame.Entry         = llvm::BasicBlock::Create( Context, "entry", Fn );
    Frame.bReturnsValue = not Result->isVoidTy();
    Frame.bClosure      = true;
    Builder->SetInsertPoint( Frame.Entry );

    llvm::Argument *Environment = Fn->getArg( static_cast<unsigned>( Fn->arg_size() - 1 ) );
    Environment->setName( "env" );
    BindCaptures( Env, Environment );

    unsigned Index = 0;
    for ( const Frontend::ParamId ParamRef : Params )
    {
        const Frontend::Param &Declared = Unit.Ast->GetParam( ParamRef );
        llvm::Argument *Arg             = Fn->getArg( Index );
        Arg->setName( Unit.Ast->Text( Declared.Name ) );
        ++Index;

        // Same rule as a method's parameters, read from the same place the
        // signature above read it: the binding site's own layout.
        const Sema::BindingSite Site{ ParamRef };
        const bool bByAddress = IsAggregate( LayoutOfValue( Values, Values.SiteType( Site ) ) );
        if ( not BindParameter( Site, Arg, bByAddress, Unit.Ast->Text( Declared.Name ) ) )
        {
            static_cast<void>(
                Fail( "llvm: closure parameter '" + std::string( Unit.Ast->Text( Declared.Name ) ) + "' has no storage" ) );
            break;
        }
    }

    if ( not Failed() )
    {
        if ( Stmts != nullptr )
        {
            // A `do ... end` body is a statement list, so it gets the same tail
            // rule a method body does: its last expression is its value.
            EmitStmts( *Stmts, Frame.bReturnsValue );
        }
        else if ( llvm::Value *Value = EmitExpr( Body ); Value != nullptr and Frame.bReturnsValue and not Terminated() )
        {
            static_cast<void>( Builder->CreateRet( CoerceWidth( Value, Result ) ) );
        }
    }

    if ( not Failed() and not Terminated() )
    {
        // Same reading as a method falling off its end: Volt has no
        // definite-return analysis, so `unreachable` is what "the middle-end
        // promised control never gets here" lowers to.
        if ( Frame.bReturnsValue )
        {
            static_cast<void>( Builder->CreateUnreachable() );
        }
        else
        {
            static_cast<void>( Builder->CreateRetVoid() );
        }
    }

    Frame = std::move( Enclosing );
    Builder->restoreIP( At );
    return Failed() ? nullptr : Fn;
}

void Volt::Backend::Llvm::LlvmBackend::State::BindCaptures ( const Sema::ClosureEnvFrame &Env, llvm::Value *Environment )
{
    llvm::Type *Address = llvm::PointerType::get( Context, 0 );
    for ( const Sema::ClosureEnvField &Field : Env.Fields )
    {
        const std::string Name = std::string( Frame.Unit->Ast->Text( Field.Name ) );
        llvm::Value *At = Builder->CreateConstInBoundsGEP1_64( Builder->getInt8Ty(), Environment, Field.Offset, "env." + Name );
        // The env holds the capture's address, so loading one yields exactly
        // what every other slot in this frame is: a place.
        Frame.Slots.emplace( Field.Site, Builder->CreateLoad( Address, At, Name ) );
    }
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitClosureEnv ( Frontend::ExprId Id, const Sema::ClosureEnvFrame &Env )
{
    llvm::Type *Address = llvm::PointerType::get( Context, 0 );
    if ( Env.Fields.Size() == 0 )
    {
        // Captures nothing, so there is nothing to point at. Null is a real
        // environment here, not a failure: the body never reads a field of it.
        return llvm::Constant::getNullValue( Address );
    }

    if ( Env.bEscapes )
    {
        // A heap environment is a call to the stdlib's annotated allocator
        // (abi.md: "heap" is a linked symbol, not compiler behaviour) — and
        // nothing in the middle-end says *which* member that is. `@[External]`
        // records a C symbol per member, but no annotation marks one as the
        // allocation entry point, so there is no allocator to call without the
        // emitter picking a name, which rules/zero-hardcode.md forbids.
        static_cast<void>( Fail( "llvm: the closure at expression " + std::to_string( Id.Value ) + " escapes and captures " +
                                 std::to_string( Env.Fields.Size() ) +
                                 " binding(s), so its environment must be heap-allocated — the middle-end marks no allocation "
                                 "entry point, and a backend may not name one" ) );
        return nullptr;
    }

    // Does not escape (ScopeResolver proved it: a literal consumed at its call
    // site), so the environment lives in the caller's frame — zero heap, which
    // is the common `arr.each do | i | ... end` case.
    llvm::AllocaInst *Storage = MakeTemp( llvm::ArrayType::get( Builder->getInt8Ty(), Env.TotalSize ), "closure.env" );
    if ( Storage == nullptr )
    {
        static_cast<void>( Fail( "llvm: no storage for the closure environment at expression " + std::to_string( Id.Value ) ) );
        return nullptr;
    }
    Storage->setAlignment( llvm::Align( Env.Alignment ) );

    const std::size_t Pointer = Mod->getDataLayout().getPointerSize();
    for ( const Sema::ClosureEnvField &Field : Env.Fields )
    {
        // The frame's fields are pointer-sized because a capture is stored as
        // an address. A frame laid out for something else would silently write
        // past its own field, so the two are checked against each other rather
        // than assumed to agree.
        if ( Field.Offset + Pointer > Env.TotalSize )
        {
            static_cast<void>( Fail( "llvm: closure environment field at offset " + std::to_string( Field.Offset ) +
                                     " does not fit in the " + std::to_string( Env.TotalSize ) +
                                     " bytes SynthesizeClosureFrame reserved" ) );
            return nullptr;
        }

        const auto Captured = Frame.Slots.find( Field.Site );
        if ( Captured == Frame.Slots.end() )
        {
            static_cast<void>( Fail( "llvm: the closure at expression " + std::to_string( Id.Value ) + " captures '" +
                                     std::string( Frame.Unit->Ast->Text( Field.Name ) ) +
                                     "', which has no slot in the enclosing function" ) );
            return nullptr;
        }

        const std::string Name = std::string( Frame.Unit->Ast->Text( Field.Name ) );
        llvm::Value *At = Builder->CreateConstInBoundsGEP1_64( Builder->getInt8Ty(), Storage, Field.Offset, "env." + Name );
        static_cast<void>( Builder->CreateStore( Captured->second, At ) );
    }
    return Storage;
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

    llvm::Value *Pair = EmitExpr( Receiver );
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

    // Always Volt code behind a callable — never external — so the
    // post-call check always runs, same as an ordinary resolved call.
    EmitExceptionCheck();
    return Call;
}
