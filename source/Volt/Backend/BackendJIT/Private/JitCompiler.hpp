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

#include <cstddef>
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

            // --- Inspection ---------------------------------------------------

            // How many generations are still mapped. A REPL shows this so that
            // a leaked ephemeral generation is a number that moved rather than
            // memory nobody notices.
            [[nodiscard]] std::size_t LiveGenerations () const;

            // Keep the text of every module that goes by, so `:ir` can be
            // answered without re-emitting. Off by default: it costs a full IR
            // render per module, which a one-shot `volt run` would pay for
            // nothing.
            void RecordIr ( bool bEnable );

            [[nodiscard]] std::string LastIr () const;

            // Machine code at `Address` as assembly text, decoding until a
            // return instruction or `MaxBytes`, whichever comes first.
            //
            // The one place in this project that reaches for LLVM's MC layer.
            // It is here rather than beside the emitter because the bytes it
            // reads are the ones ORC actually mapped — the disassembly of what
            // is running, not of what was compiled.
            [[nodiscard]] std::string Disassemble ( std::uintptr_t Address, std::size_t MaxBytes, std::string &OutError ) const;

        private:

            struct Impl;
            std::unique_ptr<Impl> P;
        };

    } // namespace Jit

} // namespace Backend

} // namespace Volt
