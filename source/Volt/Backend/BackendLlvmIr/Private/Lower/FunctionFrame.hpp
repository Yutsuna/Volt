#pragma once

// FunctionFrame.hpp — what one function body needs while it is being emitted,
// and nothing beyond it.
//
// This used to be a *member* of the emitter state, cleared by assignment before
// and after every body (`Frame = FunctionFrame{}`). It is now a local of
// whoever emits a body, owned for exactly that body's lifetime and handed to a
// BodyEmitter by reference — so a slot left over from the previous function
// cannot resolve a name to storage that no longer exists, structurally rather
// than by remembering to clear it.

#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/MiddleEnd/TypeSystem/Instantiate.hpp"

#include "Core/LlvmFwd.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        // A loop's two exits, so `break` and `next` are a branch to a block
        // this stack already knows rather than a search back up the AST.
        struct LoopFrame
        {

            llvm::BasicBlock *Latch = nullptr; // `next` — the condition test
            llvm::BasicBlock *Merge = nullptr; // `break` — past the loop
        };

        struct FunctionFrame
        {

            llvm::Function *Fn = nullptr;
            // The `alloca` backing each local, keyed by the BindingSite
            // ScopeResolver recorded. mem2reg promotes these away, which is why
            // the emitter never builds SSA itself.
            // The value is an *address*, not necessarily an alloca: an aggregate
            // parameter arrives as a pointer to the caller's storage (abi.md)
            // and is already its own slot, so copying it into one would only add
            // a memcpy nothing reads.
            std::unordered_map<MiddleEnd::Resolver::BindingSite, llvm::Value *, MiddleEnd::Resolver::BindingSiteHash> Slots;
            std::vector<LoopFrame> Loops;
            // The innermost enclosing `begin`'s dispatch block, in *this*
            // function only — Tier 1 exceptions never cross a call boundary by
            // unwinding, so a call into another llvm::Function starts with an
            // empty stack of its own (backend/llvm.md). A `raise`, or a call the
            // post-call check finds pending, branches to `.back()`; an empty
            // stack means "no handler in this function", which is the poisoned
            // early return instead.
            std::vector<llvm::BasicBlock *> Rescues;
            // The unit being walked: Ast and Scopes always come from it (a
            // generic body's lexical structure does not change under
            // instantiation, only its types do), and a call into another unit
            // reads that unit's view instead.
            const UnitView *Unit = nullptr;
            // Where an expression's type and a call's resolution come from. A
            // concrete body reads its own unit's (Unit->Values/Callees, set
            // alongside Unit); a monomorphised one reads the per-request overlay
            // MiddleEnd::TypeSystem::ReinstantiateBody returned, since UnitTypes/UnitCallees hold
            // one answer per ExprId and a generic body's ExprIds are shared by
            // every instantiation.
            const MiddleEnd::TypeSystem::UnitTypes *Values = nullptr;
            const MiddleEnd::IR::UnitCallees *Callees      = nullptr;
            // Non-null only for a monomorphised body (MonoBodyEmitter):
            // `original literal ExprId -> replacement subtree`, built by
            // MiddleEnd::TypeSystem::ReinstantiateBody for every Lambda/Block literal it found
            // still un-lowered (Instantiate.hpp's ExprRedirectMap). EmitExpr
            // consults this before reading Ast.Expr( Id ) at all, so the
            // shared literal's own slot never has to be mutated.
            const MiddleEnd::IR::ExprRedirectMap *Redirects = nullptr;
            // Where every `alloca` goes, whatever block the walk is in when it
            // needs one. Keeping them all in the entry block is what lets
            // mem2reg promote them, which is why the emitter never builds SSA.
            llvm::BasicBlock *Entry = nullptr;
            // The receiver, when the method has one, plus the instantiation it
            // was resolved at — an `@x` is a GEP into *this* shape.
            llvm::Value *Self = nullptr;
            MiddleEnd::TypeSystem::NominalId Owner;
            std::vector<std::uint32_t> OwnerArgs;
            MiddleEnd::TypeSystem::LayoutId SelfLayout;
            // Non-void functions return the value of their last expression, so a
            // tail ExprStmt emits `ret` rather than dropping its value.
            bool bReturnsValue = false;
            // True while a closure body is being emitted. Two things read it:
            // `self` and `@x`, which have no receiver here because
            // ClosureEnvFrame captures *bindings* and records no receiver, so
            // the failure names that hole rather than "outside a method"; and
            // `next`, which in a closure with no enclosing loop is the block's
            // own result, not a loop continuation.
            bool bClosure = false;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
