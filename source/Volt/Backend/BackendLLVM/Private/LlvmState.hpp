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
            // The unit being walked: types, callees and scopes all come from
            // it, and a call into another unit reads that unit's view instead.
            const UnitView *Unit = nullptr;
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
    // Mangled symbol -> the declaration created by the declare sweep. Keyed by
    // the symbol rather than by DeclId because a DeclId is only meaningful
    // inside the arena that minted it, while a symbol is the cross-unit
    // currency the linker uses too.
    std::unordered_map<std::string, llvm::Function *> Functions;

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
    [[nodiscard]] llvm::Value *CoerceWidth ( llvm::Value *Value, llvm::Type *Target );

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
    // where the receiver and the operands come from).
    [[nodiscard]] llvm::Value *EmitResolvedCall ( Frontend::ExprId Id,
                                                  const Sema::CalleeEntry &Entry,
                                                  Frontend::ExprId Receiver,
                                                  std::span<const Frontend::ExprId> Args );
};
