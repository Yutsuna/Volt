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
            // It also starts faster: ORC materialises a module the first time
            // something in it is looked up, so splitting the build lets the
            // code a run never reaches go through no codegen at all.
            bool bPerUnitModules = true;

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
            [[nodiscard]] ReloadResult Reload ( const UnitView &Unit ) override;
            [[nodiscard]] RunResult EvalUnit ( const UnitView &Unit ) override;
            [[nodiscard]] std::uintptr_t LookupSymbol ( std::string_view Mangled ) override;

        private:

            struct State;
            std::unique_ptr<State> Impl;
        };

    } // namespace Jit

} // namespace Backend

} // namespace Volt
