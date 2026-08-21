#pragma once

// JitCompiler.hpp — the ORC session, and nothing else.
//
// One LLJIT, its main dylib, and the resource trackers that make a batch of
// modules removable as a unit. Everything LLVM-and-ORC-shaped lives behind this
// class so JitBackend.cpp reads as the protocol it implements rather than as
// ORC bookkeeping.
//
// A *generation* is one batch added together and removed together. `volt run`
// only ever opens one; hot reload and the REPL are what the concept is for.

#include "Volt/BackendLlvmIr/LlvmAccess.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Volt
{

namespace Backend
{

    namespace Jit
    {

        using GenerationId = std::uint32_t;

        class JitCompiler
        {

        public:

            JitCompiler ();
            ~JitCompiler ();

            JitCompiler ( const JitCompiler & )           = delete;
            JitCompiler &operator=( const JitCompiler & ) = delete;

            // Builds the LLJIT. False with OutError set on failure — an ORC
            // Error is consumed here and never allowed to escape as one.
            [[nodiscard]] bool Init ( unsigned CompileThreads, std::string &OutError );

            // Both come from LLJIT, not from the host: the module has to be
            // typed for the machine that will actually run it.
            [[nodiscard]] std::string TargetTriple () const;
            [[nodiscard]] std::string DataLayoutString () const;

            [[nodiscard]] GenerationId OpenGeneration ();

            // A generation in a JITDylib of its own, linked against the main
            // one. What a reload needs: the replacement defines symbols the
            // running program already defines, and ORC rejects a duplicate
            // definition inside one dylib while accepting it across two. The
            // replacement lands beside the original rather than on top of it,
            // and which of the two anyone reaches is decided by the
            // indirection slot, not by the symbol table.
            [[nodiscard]] bool OpenReplacement ( GenerationId &OutGen, std::string &OutError );

            // Adds a whole emission at once: the modules, and the context
            // they share. A module that defines nothing is dropped rather than
            // added — under PerUnit granularity most of them define nothing
            // (every unit whose code came from a dylib), and a materialisation
            // unit that can never be asked for anything is pure bookkeeping.
            [[nodiscard]] bool AddModules ( GenerationId Gen, Ir::OwnedModules Modules, std::string &OutError );

            // Unmaps a generation's code. Forbidden while any frame may still
            // be inside it.
            [[nodiscard]] bool DropGeneration ( GenerationId Gen, std::string &OutError );

            // Forces materialisation and yields the address.
            [[nodiscard]] bool Lookup ( std::string_view Symbol, std::uintptr_t &OutAddr, std::string &OutError );

            // The same, resolved from inside one generation's own dylib, so a
            // symbol a replacement redefined resolves to the replacement.
            [[nodiscard]] bool
            LookupIn ( GenerationId Gen, std::string_view Symbol, std::uintptr_t &OutAddr, std::string &OutError );

            // Resolve undefined symbols against a shared object.
            [[nodiscard]] bool AddDylib ( std::string_view Path, std::string &OutError );

            // ... and against the process itself: libc, and the compiler's own
            // __volt_unwind_slots when no stdlib provides it.
            [[nodiscard]] bool AddProcessSymbols ( std::string &OutError );

        private:

            struct Impl;
            std::unique_ptr<Impl> P;
        };

    } // namespace Jit

} // namespace Backend

} // namespace Volt
