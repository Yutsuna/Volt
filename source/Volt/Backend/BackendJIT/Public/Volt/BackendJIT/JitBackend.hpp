#pragma once

// JitBackend.hpp — running a Volt build instead of writing it to disk.
//
// The same middle-end output BackendLLVM turns into an object file, handed to
// LLVM's ORC instead: the IR comes from BackendLlvmIr, exactly the IR the AOT
// path emits, and everything below is the tail that materialises and calls it.
// This module never touches BackendLLVM — the two are siblings over the shared
// emission layer, not a base and a variant.
//
// ZERO LLVM headers, like every Public/ header in the backend tree: the Driver
// includes this to run a program and must not acquire an LLVM dependency to do
// it (rules/shared-lib-exports.md).

#include "BackendJIT_export.hpp"

#include "Volt/BackendCore/ExecutableBackend.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Backend
{

    namespace Jit
    {

        struct JitOptions
        {

            // Shared objects to resolve undefined symbols against, in order.
            // The precompiled stdlib goes here when there is one; the process's
            // own symbols are always consulted last.
            std::vector<std::string> Dylibs;

            // Leading units to declare but never define, because a dylib above
            // already contains their code.
            std::uint32_t SkipUnitsBelow = 0;

            // The Volt free function control starts at, and the C-shaped symbol
            // wrapping it. The wrapper is not called `main`: this JIT runs
            // inside the compiler's own process, which already has one.
            std::string EntryFunction = "__volt_entry";
            std::string EntrySymbol   = "__volt_jit_main";

            // One llvm::Module per unit instead of one for the build. The
            // default, for two reasons that point the same way.
            //
            // It is the only shape a reload or a REPL line can have, since
            // replacing a unit means replacing a module — so running the
            // default path is running the path --watch and the REPL take, and
            // a divergence between them cannot hide.
            //
            // It also starts faster — though far less than it looks. ORC
            // materialises a module the first time anything in it is looked
            // up, so splitting the build only defers a module nothing
            // *references*: `_V_init_all` names every unit that has top-level
            // statements, and a resolved relocation drags in the callee's
            // whole module. Deferring a single function is bLazyCompilation's
            // job, not this one's.
            bool bPerUnitModules = true;

            // Compile a function the first time it is called rather than when
            // its module is first reached, through ORC's CompileOnDemandLayer.
            //
            // The measurement that motivates it: on a program of 400 functions
            // whose entry point reaches one, materialisation costs 773 ms, of
            // which 745 ms is codegen for code that never runs — 1.87 ms per
            // never-called function, linear, against a 28 ms floor.
            //
            // Off by default because it is not free and not universal. It
            // costs a stub and a call-through per function, so a program that
            // does call everything pays for the machinery and gets nothing;
            // a lazy generation cannot be dropped; and the address a symbol
            // resolves to is a stub rather than a body, which is the wrong
            // answer for `:asm` and for a reload patching a slot. `volt run`
            // turns it on, and everything holding one of those three
            // requirements leaves it off.
            bool bLazyCompilation = false;

            // Reach every callee this build defines through `@volt.fn.<sym>`
            // rather than by a direct relocation, so a reload can repoint a
            // function with one pointer store and no caller is recompiled.
            //
            // Off for a plain `volt run`: it costs a load per call and buys
            // nothing a one-shot run can use. `--watch` and the REPL turn it on,
            // and Reload refuses outright without it — an indirection that was
            // not emitted cannot be added afterwards.
            bool bIndirectLinkage = false;

            std::uint8_t OptLevel = 0;

            // 0 keeps compilation synchronous, which is what a one-shot
            // `volt run` wants: there is nothing to overlap it with.
            unsigned CompileThreads = 0;
        };

        class BACKENDJIT_EXPORT JitBackend final : public IJitBackend
        {

        public:

            JitBackend ();
            ~JitBackend () override;

            JitBackend ( const JitBackend & )           = delete;
            JitBackend &operator=( const JitBackend & ) = delete;
            JitBackend ( JitBackend && ) noexcept;
            JitBackend &operator=( JitBackend && ) noexcept;

            void SetOptions ( JitOptions InOptions );

            // --- TargetBackend ----------------------------------------------
            [[nodiscard]] std::string_view Name () const override
            {
                return "jit";
            }

            void Begin ( const BackendInput &Input ) override;
            [[nodiscard]] EEmitStatus EmitUnit ( const UnitView &Unit ) override;

            // Materialises the module into the JIT. The artifact of a JIT build
            // is the resident code itself, so EmitResult::Artifact stays empty.
            [[nodiscard]] EmitResult Finalize () override;

            // --- IJitBackend --------------------------------------------------
            [[nodiscard]] RunResult Run ( std::span<const std::string_view> ProgramArgs ) override;
            [[nodiscard]] ReloadResult Reload ( const BackendInput &Build, const UnitView &Unit ) override;
            [[nodiscard]] RunResult EvalUnit ( const BackendInput &Build, const UnitView &Unit ) override;
            [[nodiscard]] std::uintptr_t LookupSymbol ( std::string_view Mangled ) override;

            [[nodiscard]] bool
            ProbeUnit ( const BackendInput &Build, const UnitView &Unit, std::string *OutIr, std::string &OutError ) override;
            [[nodiscard]] std::string LastUnitIr () const override;
            void RecordIr ( bool bEnable ) override;
            [[nodiscard]] std::string Disassemble ( std::uintptr_t Address, std::size_t MaxBytes ) override;
            [[nodiscard]] BenchResult
            BenchUnit ( const BackendInput &Build, const UnitView &Unit, std::size_t Iterations ) override;
            [[nodiscard]] std::size_t LiveGenerations () const override;

        private:

            struct State;
            std::unique_ptr<State> Impl;
        };

    } // namespace Jit

} // namespace Backend

} // namespace Volt
