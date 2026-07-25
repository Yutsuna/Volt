#pragma once

// LlvmState.hpp — everything the LLVM emitter owns, hidden behind
// LlvmBackend's pimpl.
//
// This header is the *only* place LLVM's C++ API enters Volt, and it lives
// under Private/ so nothing upstream can include it: the rest of the compiler
// never recompiles against the LLVM API, and the module stays optional
// (VOLT_ENABLE_LLVM). Every LLVM type stops here.

#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/InstanceLayout.hpp"
#include "Volt/BackendCore/LayoutEngine.hpp"
#include "Volt/BackendCore/Mangler.hpp"
#include "Volt/BackendCore/Monomorphizer.hpp"
#include "Volt/BackendLLVM/LlvmEmitter.hpp"
#include "Volt/Sema/Layout/ClosureFrame.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

        // What one function body needs while it is being emitted, and nothing
        // beyond it: cleared per function so a stale slot cannot leak into the
        // next one.
        struct FunctionFrame
        {

            llvm::Function *Fn = nullptr;
            // The `alloca` backing each local, keyed by the BindingSite
            // ScopeResolver recorded. mem2reg promotes these away, which is
            // why the emitter never builds SSA itself.
            // The value is an *address*, not necessarily an alloca: an
            // aggregate parameter arrives as a pointer to the caller's storage
            // (abi.md) and is already its own slot, so copying it into one
            // would only add a memcpy nothing reads.
            std::unordered_map<Sema::BindingSite, llvm::Value *, Sema::BindingSiteHash> Slots;
            std::vector<LoopFrame> Loops;
            // The innermost enclosing `begin`'s dispatch block, in *this*
            // function only — Tier 1 exceptions never cross a call boundary by
            // unwinding, so a call into another llvm::Function starts with an
            // empty stack of its own (backend/llvm.md). A `raise`, or a call
            // the post-call check finds pending, branches to `.back()`; an
            // empty stack means "no handler in this function", which is the
            // poisoned early return instead.
            std::vector<llvm::BasicBlock *> Rescues;
            // The unit being walked: Ast and Scopes always come from it (a
            // generic body's lexical structure does not change under
            // instantiation, only its types do), and a call into another
            // unit reads that unit's view instead.
            const UnitView *Unit = nullptr;
            // Where an expression's type and a call's resolution come from.
            // A concrete body reads its own unit's (Unit->Values/Callees,
            // set alongside Unit); a monomorphised one reads the per-request
            // overlay Sema::ReinstantiateBody returned, since UnitTypes/
            // UnitCallees hold one answer per ExprId and a generic body's
            // ExprIds are shared by every instantiation.
            const Sema::UnitTypes *Values    = nullptr;
            const Sema::UnitCallees *Callees = nullptr;
            // Where every `alloca` goes, whatever block the walk is in when it
            // needs one. Keeping them all in the entry block is what lets
            // mem2reg promote them, which is why the emitter never builds SSA.
            llvm::BasicBlock *Entry = nullptr;
            // The receiver, when the method has one, plus the instantiation it
            // was resolved at — an `@x` is a GEP into *this* shape.
            llvm::Value *Self = nullptr;
            Sema::NominalId Owner;
            std::vector<std::uint32_t> OwnerArgs;
            Sema::LayoutId SelfLayout;
            // Non-void functions return the value of their last expression, so
            // a tail ExprStmt emits `ret` rather than dropping its value.
            bool bReturnsValue = false;
            // True while a closure body is being emitted. Two things read it:
            // `self` and `@x`, which have no receiver here because
            // ClosureEnvFrame captures *bindings* and records no receiver, so
            // the failure names that hole rather than "outside a method"; and
            // `next`, which in a closure with no enclosing loop is the block's
            // own result, not a loop continuation.
            bool bClosure = false;
        };

        // How far Finalize takes the module. `--emit` stops early; the default
        // runs the whole way to a linked artifact.
        enum class EEmitStage : std::uint8_t
        {

            Ir     = 0,
            Object = 1,
            Link   = 2,
        };

        struct EmitOptions
        {

            EEmitStage Stage      = EEmitStage::Link;
            std::uint8_t OptLevel = 0;
            bool bLto             = false;
            bool bDebugInfo       = true;
            std::string OutputPath;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt

// The pimpl body, defined here rather than in one TU so every emitter TU
// shares it while the public header stays free of LLVM.
struct Volt::Backend::Llvm::LlvmBackend::State
{

    State () = default;

    State ( const State & )           = delete;
    State &operator=( const State & ) = delete;

    // --- Owned LLVM state ------------------------------------------------

    llvm::LLVMContext Context;
    std::unique_ptr<llvm::Module> Mod;
    std::unique_ptr<llvm::IRBuilder<>> Builder;
    std::unique_ptr<llvm::TargetMachine> Machine;

    // --- The middle-end's output, read-only except for layouts ------------

    const BackendInput *Build = nullptr;
    std::optional<LayoutEngine> Layouts;
    InstanceLayouts Instances;
    Monomorphizer Mono;

    // --- Caches -----------------------------------------------------------

    // LayoutId -> llvm::Type*. One entry per distinct memory shape, so an
    // aggregate is structurally created once no matter how many types share it.
    std::unordered_map<std::uint32_t, llvm::Type *> TypeCache;
    // A closure body has no mangled symbol — it is private to the module and
    // reached only through the `{ code, env }` pair — so its name is only ever
    // read by a human. This counter is what keeps those names distinct.
    std::uint32_t ClosureCount = 0;
    // Mangled symbol -> the declaration created by the declare sweep. Keyed by
    // the symbol rather than by DeclId because a DeclId is only meaningful
    // inside the arena that minted it, while a symbol is the cross-unit
    // currency the linker uses too.
    std::unordered_map<std::string, llvm::Function *> Functions;

    // --- Exceptions (ExceptionEmitter.cpp), lazily created ----------------

    // The in-flight exception: an address (`ExcValue`) plus the raised
    // object's own NominalId (`ExcTag`), thread-local because the transport is
    // per-thread state, not a value carried by any signature (abi.md reserves
    // no channel for it). `ExcTag == NominalId::InvalidValue` means "nothing
    // is in flight" — the same sentinel TypedId already uses for "no id",
    // reused rather than inventing a second one.
    llvm::GlobalVariable *ExcValue = nullptr;
    llvm::GlobalVariable *ExcTag   = nullptr;
    // `NominalId -> its immediate Super's NominalId, or InvalidValue`, one row
    // per type the store declares, built once from TypeStore::Type(Id).Super —
    // the same chain Sema's own IsSubclassOf walks (TypeResolve.cpp). A
    // `rescue` clause tests membership by walking this at runtime, since the
    // dynamic side of the test is only known once a program actually raises.
    llvm::GlobalVariable *Ancestry = nullptr;

    // --- Per-function scratch --------------------------------------------

    FunctionFrame Frame;
    EmitOptions Options;

    // --- Failure ----------------------------------------------------------

    // The first contract violation seen. A backend never diagnoses Volt source
    // (rules/core-ast.md): reaching this means the middle-end handed over
    // something its own invariants say it cannot, so the message names the
    // hole, not the user's program.
    EEmitStatus Status = EEmitStatus::Ok;
    std::string Message;

    // Record a middle-end contract violation and yield an Error status. The
    // first one wins: later failures are almost always its consequences.
    EEmitStatus Fail ( std::string InMessage );

    // True once emission has failed, so a walk can unwind without every step
    // re-checking the same condition.
    [[nodiscard]] bool Failed () const
    {
        return Status == EEmitStatus::Error;
    }

    // Create the module and the host TargetMachine. Separate from the
    // constructor because the triple is a *build* decision, and the seam for
    // cross-compilation is exactly this one string.
    [[nodiscard]] bool InitTarget ( std::string_view ModuleName );

    // --- Types (TypeMapper.cpp) ------------------------------------------

    // The LLVM type for a memory layout. Reads only Primitive{ Spelling, Bits }
    // — the compiler never learns that "f64" means any particular Volt type
    // (rules/zero-hardcode.md). Null when the layout is unresolved.
    [[nodiscard]] llvm::Type *TypeOfLayout ( Sema::LayoutId Id );

    // How a value of this layout crosses a call boundary. Scalars travel in a
    // register; an aggregate travels by pointer, which is what abi.md fixes
    // for all three targets ("aggregates by pointer, byval later if profiling
    // asks"). Null when the layout is unresolved.
    [[nodiscard]] llvm::Type *ParamTypeOfLayout ( Sema::LayoutId Id );

    // The signature of `Entry` as a member of `Owner`, instantiated for
    // `FlatArgs`. Parameter order is abi.md's, once for every target:
    // `self`, then the declared parameters. (`ptr %env` trails a *closure*
    // body, which is emitted from a Lambda, not from a declaration.)
    [[nodiscard]] llvm::FunctionType *
    FunctionTypeOf ( const Sema::Member &Entry, Sema::NominalId Owner, std::span<const std::uint32_t> FlatArgs );

    // --- Declare sweep (LlvmEmitter.cpp) ----------------------------------

    // Create an llvm::Function for every concrete method and free function of
    // the build, before any body is emitted, so a call to a callee declared in
    // another unit — or later in this one — resolves with no fixup pass.
    void DeclareAll ();

    // One declaration. Returns null when the member is not something codegen
    // emits a symbol for (abstract, generic, a field), which is not a failure.
    llvm::Function *DeclareMember ( const Sema::Member &Entry, Sema::NominalId Owner );

    // In debug builds, check that LLVM's own DataLayout agrees with
    // LayoutEngine about this aggregate's size and every field offset.
    // LayoutEngine is the single ABI authority (.agents/backend/abi.md), and a
    // silent divergence here corrupts every `@[External]` C struct with no
    // diagnostic at all — so it is checked, not assumed.
    void VerifyAggregateAbi ( Sema::LayoutId Id, llvm::StructType *Shape );

    // --- Expression types (TypeMapper.cpp) --------------------------------

    // The MonoRequest encoding of an inferred expression type: a pre-order
    // walk emitting, per node, its NominalId then its own argument count. The
    // one currency InstanceLayouts, Monomorphizer and Mangler share, so a
    // layout, a symbol and a queue entry can never mean different
    // instantiations.
    void FlattenValueType ( const Sema::UnitTypes &Values, Sema::SemaTypeId Id, std::vector<std::uint32_t> &Out ) const;

    // The memory shape of a value of this inferred type. Invalid when the type
    // is absent — which inside a generic body is normal (UnitTypes::IsDeferred)
    // and everywhere else is a middle-end hole the caller reports.
    [[nodiscard]] Sema::LayoutId LayoutOfValue ( const Sema::UnitTypes &Values, Sema::SemaTypeId Id );

    // The layout, then the llvm::Type, of what an expression evaluates to.
    [[nodiscard]] Sema::LayoutId LayoutOfExpr ( Frontend::ExprId Id );
    [[nodiscard]] llvm::Type *TypeOfExpr ( Frontend::ExprId Id );

    // An aggregate never travels in a register (abi.md): it is addressed, so
    // every expression of aggregate layout evaluates to a `ptr` at its storage
    // rather than to a loaded struct value.
    [[nodiscard]] bool IsAggregate ( Sema::LayoutId Id ) const;

    // --- Sweeps (LlvmEmitter.cpp) -----------------------------------------

    // Fill in the bodies every member of `Unit` declares. Reads the TypeStore
    // for the same reason the declare sweep does — it is the resolved
    // interface — and keeps only what `Unit.Ordinal` says this unit holds.
    void DefineAll ( const UnitView &Unit );

    // One body. Silently skips a member with no body to emit (external,
    // abstract, generic); a member whose declaration is not the Method the
    // store says it is, is a contract violation and reported.
    void DefineMember ( const Sema::Member &Entry, Sema::NominalId Owner, const UnitView &Unit );

    // The linker symbol a resolved callee is reached by: the C spelling
    // verbatim for an `@[External]` member — the whole point of that boundary
    // — and the mangled scheme otherwise.
    [[nodiscard]] std::string
    SymbolOf ( const Sema::Member &Entry, Sema::NominalId Owner, std::span<const std::uint32_t> FlatArgs ) const;

    // The declaration for that symbol, created on demand: the declare sweep
    // covers every concrete member of the build, but a monomorphised callee is
    // only named once a call site fixes its arguments.
    [[nodiscard]] llvm::Function *
    FunctionFor ( const Sema::Member &Entry, Sema::NominalId Owner, std::span<const std::uint32_t> FlatArgs );

    // --- Statements (StmtEmitter.cpp) -------------------------------------

    // Emit a statement list. `bTail` marks the list as the function's result
    // position, where a trailing expression *is* the return value.
    void EmitStmts ( const Frontend::StmtList &List, bool bTail );
    void EmitStmt ( Frontend::StmtId Id, bool bTail );

    // The storage backing a binding, created in the entry block on first
    // mention so a `LocalDecl` and every later use of it share one slot.
    [[nodiscard]] llvm::Value *SlotFor ( const Sema::BindingSite &Site, llvm::Type *Shape, std::string_view Name );

    // Unnamed entry-block storage, for a value that converges out of several
    // blocks and has no binding to key on.
    [[nodiscard]] llvm::AllocaInst *MakeTemp ( llvm::Type *Shape, std::string_view Name ) const;

    // True when the block being written into already ends in a terminator, so
    // a walk stops appending instructions after a `return` / `break`.
    [[nodiscard]] bool Terminated () const;

    // --- Expressions (ExprEmitter.cpp) ------------------------------------

    // The value of an expression: a register for a scalar, a `ptr` to the
    // storage for an aggregate. Null on failure, with Status already set.
    [[nodiscard]] llvm::Value *EmitExpr ( Frontend::ExprId Id );

    // The *address* of an assignable expression — a local slot, an `@x` GEP, a
    // field GEP, the operand of a `*p`. Null when the expression is not a
    // place, which is a middle-end contract violation and is reported.
    [[nodiscard]] llvm::Value *EmitAddress ( Frontend::ExprId Id );

    // Move a value into storage: a plain store for a scalar, a memcpy sized by
    // LayoutEngine for an aggregate, since an aggregate is only ever an
    // address.
    void EmitStore ( llvm::Value *Address, llvm::Value *Value, Sema::LayoutId Shape );

    // The report for a receiver-less `self` / `super` / `@x`. Inside a closure
    // body the hole is a different one and is named as such: ClosureEnvFrame
    // captures *bindings*, and a receiver is not one, so there is nothing for
    // the body to reach `self` through.
    [[nodiscard]] std::string MissingSelf ( std::string_view What ) const;

    // Address of a place, then the load of it — one per node kind that is both
    // a place and a value (`x`, `@x`, `o.x`, `*p`).
    [[nodiscard]] llvm::Value *LoadPlace ( Frontend::ExprId Id );

    // GEP to the field named `Name` inside an aggregate. The index is the
    // field's position in the layout, which is declaration order and therefore
    // the position TypeMapper built the LLVM struct with — VerifyAggregateAbi
    // has already checked the two agree byte for byte with LayoutEngine.
    [[nodiscard]] llvm::Value *
    FieldAddress ( llvm::Value *Object, Sema::LayoutId Shape, std::string_view Name, Frontend::ExprId Id );

    // The opaque spelling driving instruction selection for a layout: a
    // Primitive's own, and "ptr" for a Pointer, so the two shapes of address
    // cannot select different instructions. Empty for an aggregate.
    [[nodiscard]] std::string_view SpellingOf ( Sema::LayoutId Id ) const;

    // The zext TypeCompat's widening rule implies. Sema already decided an
    // `i8` is acceptable where a `u64` is expected (rules/zero-hardcode.md);
    // this only carries that decision onto the wire.
    [[nodiscard]] llvm::Value *CoerceWidth ( llvm::Value *Value, llvm::Type *Target ) const;

    // Per-node emission. Each takes the node it was dispatched on, so the
    // single std::visit site in EmitExpr stays a table of one-liners.
    [[nodiscard]] llvm::Value *EmitIntLiteral ( Frontend::ExprId Id, const Frontend::IntLiteral &Node );
    [[nodiscard]] llvm::Value *EmitFloatLiteral ( Frontend::ExprId Id, const Frontend::FloatLiteral &Node );
    [[nodiscard]] llvm::Value *EmitCharLiteral ( Frontend::ExprId Id, const Frontend::CharLiteral &Node );
    [[nodiscard]] llvm::Value *EmitStringLiteral ( Frontend::ExprId Id, const Frontend::StringLiteral &Node );
    [[nodiscard]] llvm::Value *EmitSizeOf ( Frontend::ExprId Id );
    [[nodiscard]] llvm::Value *EmitBinary ( Frontend::ExprId Id, const Frontend::Binary &Node );
    [[nodiscard]] llvm::Value *EmitUnary ( Frontend::ExprId Id, const Frontend::Unary &Node );
    [[nodiscard]] llvm::Value *EmitPointerArith ( const Frontend::Binary &Node );
    [[nodiscard]] llvm::Value *EmitShortCircuit ( const Frontend::Binary &Node );
    [[nodiscard]] llvm::Value *EmitTernary ( Frontend::ExprId Id, const Frontend::Ternary &Node );
    [[nodiscard]] llvm::Value *EmitCase ( Frontend::ExprId Id, const Frontend::CaseExpr &Node );
    [[nodiscard]] llvm::Value *EmitCall ( Frontend::ExprId Id, const Frontend::Call &Node );
    [[nodiscard]] llvm::Value *FailAggregateLiteral ( Frontend::ExprId Id, std::string_view Kind );

    // The one call site every resolved callee goes through — an explicit
    // `Call`, and a `Binary`/`Unary` whose operator resolved to a method
    // (rules/core-ast.md: the two are the same emission, told apart only by
    // where the receiver and the operands come from). `Block` is the trailing
    // `do ... end`, which fills the callee's `&block` slot rather than a
    // positional one and is invalid for every other caller.
    [[nodiscard]] llvm::Value *EmitResolvedCall ( Frontend::ExprId Id,
                                                  const Sema::CalleeEntry &Entry,
                                                  Frontend::ExprId Receiver,
                                                  std::span<const Frontend::ExprId> Args,
                                                  Frontend::ExprId Block = Frontend::ExprId{} );

    // --- Closures (ClosureEmitter.cpp) ------------------------------------

    // The closure *value*, uniform across the three backends (abi.md): the
    // two-slot `{ ptr code, ptr env }` aggregate — hence, like every
    // aggregate, an address.
    [[nodiscard]] llvm::StructType *ClosurePairType ();

    // A `( x ) => expr` and a `do | x | ... end` differ only in what their
    // body is, so both land in EmitClosure with one of the two filled.
    [[nodiscard]] llvm::Value *EmitLambda ( Frontend::ExprId Id, const Frontend::Lambda &Node );
    [[nodiscard]] llvm::Value *EmitBlock ( Frontend::ExprId Id, const Frontend::Block &Node );
    [[nodiscard]] llvm::Value *EmitClosure ( Frontend::ExprId Id,
                                             const Frontend::ParamList &Params,
                                             Frontend::ExprId Body,
                                             const Frontend::StmtList *Stmts );

    // The private function a closure body compiles to: declared parameters in
    // order, then `ptr %env` trailing (abi.md fixes that order for all three
    // targets). Emitted under a nested FunctionFrame, so the enclosing body's
    // slots and insert point are restored on the way out.
    [[nodiscard]] llvm::Function *EmitClosureBody ( Frontend::ExprId Id,
                                                    const Sema::ClosureEnvFrame &Env,
                                                    const Frontend::ParamList &Params,
                                                    Frontend::ExprId Body,
                                                    const Frontend::StmtList *Stmts );

    // Bind every capture to a slot inside the closure body: an env field holds
    // the *address* of the captured binding, so loading one yields exactly what
    // FunctionFrame::Slots holds for a local — a place.
    void BindCaptures ( const Sema::ClosureEnvFrame &Env, llvm::Value *Environment );

    // The environment, allocated in the *enclosing* function: SynthesizeClosureFrame
    // already fixed its size, alignment and every field offset, and this only
    // fills it. Null when the closure captures nothing, which is a valid env
    // no field is ever read out of.
    [[nodiscard]] llvm::Value *EmitClosureEnv ( Frontend::ExprId Id, const Sema::ClosureEnvFrame &Env );

    // `next [value]` inside a closure body with no loop of its own: the block
    // ends this invocation and hands `value` back, so it is a `ret`. Kept here
    // rather than in StmtEmitter because it is the closure protocol, not the
    // loop one — the two only share a keyword.
    void EmitBlockNext ( Frontend::ExprId Value );

    // An indirect call through a closure value: `f( x )` on a local holding a
    // callable, and `block.call( x )` on a `&block` parameter, are the same
    // emission — the callee's `@[Apply]` member says the signature is the
    // receiver's own type arguments (result first, then parameters).
    [[nodiscard]] llvm::Value *EmitApplyCall ( Frontend::ExprId Id,
                                               const Sema::CalleeEntry &Entry,
                                               Frontend::ExprId Receiver,
                                               std::span<const Frontend::ExprId> Args );

    // --- Exceptions, Tier 1 (ExceptionEmitter.cpp) ------------------------
    //
    // "setjmp/sigsetjmp-free personality-less unwinding" (llvm.md): `raise`
    // never touches the C stack directly. It stores the exception into
    // thread-local state and takes the *poisoned path* — a branch to the
    // innermost `begin` in this function, or an early return carrying no
    // value, exactly as if the raising call itself had returned. Every
    // ordinary (non-`@[External]`) call this emitter makes is followed by a
    // check of that same state, so a `raise` several calls deep unwinds one
    // `ret` at a time until some caller's frame is inside a `begin`.

    // The thread-local exception-value / exception-tag globals, created once
    // per module on first use.
    [[nodiscard]] llvm::GlobalVariable *ExceptionValueSlot ();
    [[nodiscard]] llvm::GlobalVariable *ExceptionTagSlot ();

    // `NominalId -> immediate Super`, one row per declared type, emitted once
    // as module-level constant data.
    [[nodiscard]] llvm::GlobalVariable *AncestryTable ();

    // True (i1) when `Dynamic` (a NominalId, as loaded from ExceptionTagSlot)
    // names `Target` or one of its ancestors — the same reflexive, depth-bounded
    // walk IsSubclassOf performs in Sema, done here at runtime because the
    // dynamic side is only known once a program actually raises.
    [[nodiscard]] llvm::Value *EmitAncestorTest ( llvm::Value *Dynamic, Sema::NominalId Target );

    // The exit every raising path shares once it decides *this* function does
    // not handle it: branch to `Frame.Rescues.back()` when this function has
    // an active `begin`, otherwise an early return of a poisoned value (`ret
    // void` / `ret zeroinitializer`) — the propagation step a caller's own
    // post-call check will observe.
    void EmitPoisonedPath ();

    // Checked after every call to Volt code (never after an `@[External]`
    // call, which cannot raise a Volt exception): branches to EmitPoisonedPath
    // when ExceptionTagSlot is no longer NominalId::InvalidValue, otherwise
    // falls through. Splits the current block; the caller's Builder insertion
    // point is left in the fallthrough half.
    void EmitExceptionCheck ();

    [[nodiscard]] llvm::Value *EmitRaise ( Frontend::ExprId Id, const Frontend::RaiseExpr &Node );
    [[nodiscard]] llvm::Value *EmitBegin ( Frontend::ExprId Id, const Frontend::BeginExpr &Node );

    // The shared tail rule Begin's own body and each RescueClause's body use:
    // the trailing expression of a StmtList, stored into `Slot` rather than
    // returned — `Begin`'s value converges through storage, like `CaseExpr`'s,
    // since its clauses are statement lists whose count is not fixed structure.
    void EmitBeginTail ( const Frontend::StmtList &Body, llvm::AllocaInst *Slot, llvm::Type *Shape );

    // --- Monomorphisation (MonoEmitter.cpp) --------------------------------

    // The member a request names, plus the UnitView that declares it (its
    // Ast/Scopes are what ReinstantiateBody re-types the body against). Null
    // when the store declares no such member — a middle-end contract
    // violation, reported by the caller.
    [[nodiscard]] const Sema::Member *LookupMonoMember ( const MonoRequest &Request, const UnitView **OutUnit ) const;

    // One instantiation: resolve the member, get or declare its Function
    // (FunctionFor's own cache makes this idempotent), reinstantiate the
    // body under Sema::ReinstantiateBody's overlay, and emit it exactly as
    // DefineMember emits a concrete one.
    void EmitMonomorphizedBody ( const MonoRequest &Request );

    // Every request discovered so far, and everything a drained body itself
    // discovers — a generic method calling another generic method — until
    // the queue is empty.
    void DrainMonomorphizer ();
};
