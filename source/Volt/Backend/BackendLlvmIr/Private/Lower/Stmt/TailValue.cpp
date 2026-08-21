// TailValue.cpp — the convergence rule `if`, `case` and `begin` all share.
//
// Each of them is a *statement list* per arm, so the number of blocks reaching
// the merge is only known once the whole chain is built — which rules out a phi.
// They converge through a slot instead, and both halves of that (the store on
// the way in, the load on the way out) must obey one convention, or an
// aggregate arm silently produces an SSA struct where every consumer expects an
// address. The rule lives here once rather than at the three stores.

#include "Lower/BodyEmitter.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

void Volt::Backend::Llvm::BodyEmitter::StoreTailValue ( llvm::Value *Value,
                                                        llvm::Value *Slot,
                                                        llvm::Type *Shape,
                                                        MiddleEnd::TypeSystem::LayoutId Layout )
{
    // A tail expression with no value converges nothing. The ordinary case is a
    // call to a `-> Void` member: `begin level3() rescue e : E then 7 end` in
    // statement position is valid Volt where only the rescue arm has a value, so
    // the slot is simply never written on this path. Nothing to report.
    //
    // It is also not something to hand to CreateStore. A void operand builds an
    // ill-formed instruction, and LLVM answers that by asking its DataLayout for
    // the alignment of a type that has none — which is not a diagnostic but an
    // unbounded scan inside the library. That was a compiler hang, on a
    // fifteen-line program.
    if ( Value == nullptr or Slot == nullptr or Shape == nullptr or Value->getType()->isVoidTy() )
    {
        return;
    }

    // An aggregate value is only ever an address (rules/core-ast.md): the arm
    // handed back the address of its own storage, and converging it into the
    // slot is `EmitStore`'s ordinary memcpy, exactly like assigning one String
    // to another — never a load into an SSA value of the struct type.
    if ( IsAggregate( Layout ) )
    {
        EmitStore( Slot, Value, Layout );
        return;
    }

    // Any *other* disagreement is a real one: the slot's type is the one Sema
    // gave the whole `begin`/`case`, so a value that does not fit it after
    // coercion means the arms were typed inconsistently. Named here rather than
    // stored through an opaque pointer, which is exactly how a mismatched width
    // becomes silent corruption (the reason EmitStore reconciles centrally).
    llvm::Value *Fitted = CoerceWidth( Value, Shape );
    if ( Fitted == nullptr or Fitted->getType() != Shape )
    {
        std::string ValueText;
        std::string SlotText;
        llvm::raw_string_ostream ValueStream( ValueText );
        llvm::raw_string_ostream SlotStream( SlotText );
        ( Fitted != nullptr ? Fitted->getType() : Value->getType() )->print( ValueStream );
        Shape->print( SlotStream );
        static_cast<void>(
            Fail( "llvm: a `begin`/`case` arm yields " + ValueStream.str() + " but the result slot is " + SlotStream.str() +
                  " — the arms of one expression were typed inconsistently (fn: " + Frame().Fn->getName().str() + ")" ) );
        return;
    }
    static_cast<void>( Ctx().Builder().CreateStore( Fitted, Slot ) );
}

llvm::Value *Volt::Backend::Llvm::BodyEmitter::LoadConverged ( llvm::Value *Slot,
                                                               llvm::Type *Shape,
                                                               MiddleEnd::TypeSystem::LayoutId Layout,
                                                               const char *Name )
{
    if ( Slot == nullptr or Shape == nullptr )
    {
        return nullptr;
    }

    // Loading an aggregate out of the slot would produce an SSA struct, and
    // every consumer of an aggregate expects an address instead: `EmitStore`
    // memcpys from it, a call passes it byval. Handing back the loaded struct
    // built a memcpy whose operand was `{ ptr, i64 }` rather than `ptr`, which
    // the module verifier rejects — the same convention StoreTailValue applies
    // on the way in, applied on the way out.
    if ( IsAggregate( Layout ) )
    {
        return Slot;
    }
    return Ctx().Builder().CreateLoad( Shape, Slot, Name );
}
