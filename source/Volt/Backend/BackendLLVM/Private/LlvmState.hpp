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
            std::unordered_map<Sema::BindingSite, llvm::AllocaInst *, Sema::BindingSiteHash> Slots;
            std::vector<LoopFrame> Loops;
            // The unit being walked: types, callees and scopes all come from
            // it, and a call into another unit reads that unit's view instead.
            const UnitView *Unit = nullptr;
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
};
