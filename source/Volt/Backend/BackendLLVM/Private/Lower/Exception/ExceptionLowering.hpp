#pragma once

// ExceptionLowering.hpp — `RaiseExpr` / `BeginExpr`, Tier 1.
//
// backend/llvm.md: "setjmp/sigsetjmp-free personality-less unwinding is not
// attempted". `raise` never touches the C stack directly — it stores the
// exception into thread-local state and takes the *poisoned path*: a branch to
// the innermost `begin` this function owns, or an early return carrying no
// value, exactly as if the raising call itself had simply returned. Every
// ordinary call this emitter makes is followed by a check of that same state,
// so a `raise` several calls deep unwinds one `ret` at a time until some
// caller's frame is inside a `begin`. Simple, portable, correct, and — because
// it never reserves a channel in any signature — it costs the calling
// convention nothing (abi.md).
//
// The split of this class: it *owns* the module-level state (the thread-local
// globals, the storage buffer and its size, the ancestry table), which is
// created once per module and outlives any one body. Everything that emits into
// a body takes a `BodyEmitter &` instead, because that is where the frame is.

#include "Core/EmitterServices.hpp"
#include "Core/LlvmFwd.hpp"

#include <cstddef>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class BodyEmitter;

        class ExceptionLowering
        {

        public:

            explicit ExceptionLowering ( EmitterServices &InServices ) : Services( &InServices )
            {
            }

            // --- Module-level state (ExceptionGlobals.cpp, ExceptionStorage.cpp,
            //     AncestryTable.cpp), all lazily created on first use ----------

            // The in-flight exception: an address (`volt.exc.value`) plus the
            // raised object's own NominalId (`volt.exc.tag`), thread-local
            // because the transport is per-thread state, not a value carried by
            // any signature (abi.md reserves no channel for it). `ExcTag ==
            // NominalId::InvalidValue` means "nothing is in flight" — the same
            // sentinel TypedId already uses for "no id", reused rather than
            // inventing a second one.
            [[nodiscard]] llvm::GlobalVariable *ExceptionValueSlot ();
            [[nodiscard]] llvm::GlobalVariable *ExceptionTagSlot ();

            // The thread-local `i1` a non-local `break` sets before taking the
            // poisoned path, and the one thing that distinguishes "unwinding
            // because of a break" from "unwinding because of an exception" once
            // both have collapsed onto the same branch-to-Rescues protocol. A
            // sentinel folded into `ExcTag` would make the unhandled-exception
            // path misreport a runaway `break` as an exception, and would
            // corrupt AncestryTable's lookup by a NominalId that was never one.
            [[nodiscard]] llvm::GlobalVariable *BreakFlagSlot ();

            // Where the in-flight object itself lives. It cannot stay in the
            // raising frame: tier 1 propagates by *returning*, so by the time a
            // `rescue` several frames up copies it out — or the last-resort hook
            // reports it, with every Volt frame already gone — that alloca is
            // dead storage. So `raise` copies the object here first and
            // publishes *this* address, and the buffer is sized once for the
            // widest type that can reach it (every descendant of the
            // `@[Literal( RaiseExpr )]`), measured by LayoutEngine.
            [[nodiscard]] llvm::GlobalVariable *ExceptionStorageSlot ();

            [[nodiscard]] std::size_t StorageSize () const noexcept
            {
                return ExcStorageSize;
            }

            // `NominalId -> its immediate Super's NominalId, or InvalidValue`,
            // one row per type the store declares, built once from
            // TypeStore::Type(Id).Super — the same chain Sema's own IsSubclassOf
            // walks. A `rescue` clause tests membership by walking this at
            // runtime, since the dynamic side of the test is only known once a
            // program actually raises.
            [[nodiscard]] llvm::GlobalVariable *AncestryTable ();

            // --- Emission into a body -----------------------------------------

            // True (i1) when `Dynamic` (a NominalId, as loaded from
            // ExceptionTagSlot) names `Target` or one of its ancestors — the
            // same reflexive, depth-bounded walk IsSubclassOf performs in Sema,
            // done here at runtime because the dynamic side is only known once a
            // program actually raises.
            [[nodiscard]] llvm::Value *EmitAncestorTest ( BodyEmitter &Emitter, llvm::Value *Dynamic, Sema::NominalId Target );

            // The exit every raising path shares once it decides *this* function
            // does not handle it: branch to `Frame.Rescues.back()` when this
            // function has an active `begin`, otherwise an early return of a
            // poisoned value — the propagation step a caller's own post-call
            // check will observe.
            void EmitPoisonedPath ( BodyEmitter &Emitter );

            // Checked after every call to Volt code (never after an
            // `@[External]` call, which cannot raise a Volt exception): branches
            // to the poisoned path when the tag is no longer InvalidValue,
            // otherwise falls through. Splits the current block; the caller's
            // Builder insertion point is left in the fallthrough half.
            //
            // Exception-only, deliberately: EmitResolvedCall reads this alone
            // for a call it bound a trailing block to, because *that* call site
            // is where a `break` inside the block is consumed, not propagated
            // further — see EmitUnwindCheck for the sites that propagate both.
            void EmitExceptionCheck ( BodyEmitter &Emitter );

            // `EmitExceptionCheck`'s superset: also propagates a `break` in
            // flight, for every call site that is not the one specific
            // block-consumer above — an ordinary call, and `block.call(...)`,
            // which must let a `break` the block contains keep unwinding through
            // it rather than swallowing it.
            void EmitUnwindCheck ( BodyEmitter &Emitter );

            [[nodiscard]] llvm::Value *EmitRaise ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::RaiseExpr &Node );
            [[nodiscard]] llvm::Value *EmitBegin ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::BeginExpr &Node );

            // The shared tail rule Begin's own body and each RescueClause's body
            // use: the trailing expression of a StmtList, stored into `Slot`
            // rather than returned — `Begin`'s value converges through storage,
            // like `CaseExpr`'s, since its clauses are statement lists whose
            // count is not fixed structure.
            void EmitBeginTail ( BodyEmitter &Emitter,
                                 const Frontend::StmtList &Body,
                                 llvm::AllocaInst *Slot,
                                 llvm::Type *Shape,
                                 Sema::LayoutId Layout );

        private:

            EmitterServices *Services = nullptr;

            llvm::GlobalVariable *ExcValue   = nullptr;
            llvm::GlobalVariable *ExcTag     = nullptr;
            llvm::GlobalVariable *BrkFlag    = nullptr;
            llvm::GlobalVariable *ExcStorage = nullptr;
            std::size_t ExcStorageSize       = 0;
            llvm::GlobalVariable *Ancestry   = nullptr;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
