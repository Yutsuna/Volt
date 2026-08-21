#pragma once

// IrGenerator.hpp — the AST-to-LLVM-IR layer, as an interface with no LLVM in
// it.
//
// Everything under this module's Private/ turns a middle-end BackendInput into
// an llvm::Module. Two very different tails consume that module: BackendLLVM
// optimises it and writes a `.o`, BackendJIT hands it to ORC and calls into it.
// Neither tail is a variant of the other, and neither may reach into the other,
// so the emission layer lives here and both depend on it.
//
// ZERO LLVM headers, deliberately (rules/shared-lib-exports.md): this header is
// what the Driver and any tool would include, and it must not drag tens of
// thousands of lines of template code behind it. The consumers that genuinely
// need llvm:: types include LlvmAccess.hpp beside it — a separate header
// precisely so that including this one stays cheap.
//
// The knobs below are the *whole* difference between an AOT emission and a JIT
// one. That is the load-bearing claim of this module: if a behaviour needed by
// one tail cannot be expressed as one of these fields, it does not belong in
// the shared layer.

#include "BackendLlvmIr_export.hpp"

#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/TargetBackend.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Backend
{

    namespace Ir
    {

        // How many llvm::Modules the emission produces.
        enum class EModuleGranularity : std::uint8_t
        {
            // One module for the whole build. The simplest correct thing, and
            // what both AOT and the first JIT milestone use.
            Whole = 0,

            // One module per unit, plus a prelude module holding the shared
            // globals and the entry point. What hot reload and the REPL need,
            // because ORC can only replace a module as a unit.
            PerUnit = 1,
        };

        // How the three unwind-transport slots (UnwindTransport.hpp) are
        // addressed.
        enum class ETlsAccess : std::uint8_t
        {
            // thread_local globals, addressed directly. What a linked object
            // does, and what the static linker resolves for free.
            Direct = 0,

            // A call to UnwindTransport::SlotAccessorSymbol, then a gep into
            // the block it returns. The JIT uses this because emitting real TLS
            // relocations into JIT-linked code would pull in an ORC runtime
            // library, and a `ptr()` call needs nothing but a symbol.
            Accessor = 1,
        };

        // How an emitted call reaches its callee.
        enum class ELinkage : std::uint8_t
        {
            // `call @sym`. Resolved once, never movable afterwards.
            Direct = 0,

            // `load ptr @volt.fn.sym` then call through it. One indirection per
            // call, in exchange for being able to repoint a function without
            // recompiling its callers — the hot-reload seam.
            Indirect = 1,
        };

        struct IrOptions
        {

            EModuleGranularity Granularity = EModuleGranularity::Whole;
            ETlsAccess Tls                 = ETlsAccess::Direct;
            ELinkage Linkage               = ELinkage::Direct;

            // Units whose ordinal is below this are declared but never defined:
            // their code already exists in a precompiled artifact. The JIT sets
            // it when it loads the stdlib as a shared object.
            std::uint32_t SkipUnitsBelow = 0;

            // ... except for bodies the caller still wants available to the
            // inliner. The AOT build links the same precompiled stdlib archive,
            // but still emits its inline-eligible bodies so an -O2 run can
            // inline across the boundary. Independent of SkipUnitsBelow because
            // the JIT wants the skip without the exception.
            bool bDefineInlineEligibleBelow = false;

            std::string TargetTriple; // empty -> host
            std::string DataLayout;   // empty -> derived from the TargetMachine

            std::string EntryFunction = "__volt_entry";

            // The CRT-visible symbol wrapping EntryFunction. Empty means no
            // shim at all — a shared library, or a JIT that calls the entry
            // function directly.
            std::string EntrySymbol = "main";

            // AOT leaves this off and verifies in its own pipeline, where the
            // failure is reported with the offending function named. A JIT has
            // no such pipeline, so it verifies here.
            bool bVerify = false;

            bool bDebugInfo = true;

            // A TargetMachine is only needed by a consumer that will run
            // addPassesToEmitFile. The JIT has none and wants none.
            bool bNeedTargetMachine = true;
        };

        // Emission is a three-phase protocol, in this order: Begin once, EmitUnit
        // per unit in circuit link order, Finish once. Nothing may be emitted
        // after Finish.
        class BACKENDLLVMIR_EXPORT IrGenerator
        {

        public:

            explicit IrGenerator ( IrOptions InOptions );
            ~IrGenerator ();

            IrGenerator ( const IrGenerator & )           = delete;
            IrGenerator &operator=( const IrGenerator & ) = delete;
            IrGenerator ( IrGenerator && ) noexcept;
            IrGenerator &operator=( IrGenerator && ) noexcept;

            // Create the module and declare everything the build can reach.
            // Declaring up front is what lets a body in the first unit call
            // something declared in the last with no fixup pass.
            void Begin ( const BackendInput &Input );

            [[nodiscard]] EEmitStatus EmitUnit ( const UnitView &Unit );

            // Drain the monomorphisation queue to a fixpoint, emit _V_init_all
            // and the entry point, then verify if asked. After this returns Ok
            // the module is complete and never grows again.
            [[nodiscard]] EEmitStatus Finish ();

            // How wide the in-flight-exception buffer has to be for this
            // build. Only an ETlsAccess::Accessor consumer needs it: it owns
            // the storage the accessor table points at, and this is the only
            // place that knows how large the widest raisable type is.
            [[nodiscard]] std::size_t UnwindStorageSize () const;

            [[nodiscard]] bool Failed () const noexcept;
            [[nodiscard]] std::string_view Error () const noexcept;

            // The symbols the last EmitUnit defined. A hot-reload consumer uses
            // this to know which indirection slots to repoint.
            [[nodiscard]] std::vector<std::string> LastUnitSymbols () const;

            // Incomplete here on purpose — this is what keeps LLVM out of
            // this header. LlvmAccess.hpp completes it for the two consumers
            // that need llvm:: types.
            struct State;

            [[nodiscard]] State *Peek () const noexcept
            {
                return Impl.get();
            }

        private:

            std::unique_ptr<State> Impl;
        };

    } // namespace Ir

} // namespace Backend

} // namespace Volt
