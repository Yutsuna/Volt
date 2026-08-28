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

namespace Volt
{

namespace Backend
{

    namespace Jit
    {

        using GenerationId = std::uint32_t;

        // When a function's machine code is produced.
        //
        // ORC materialises a whole *module* the first time anything in it is
        // looked up, and resolution is transitive — one reachable function
        // drags its module in, and every symbol that module references drags
        // theirs. `_V_init_all` alone reaches every unit that has top-level
        // statements, so Eager compiles essentially the whole program before
        // its first instruction runs.
        //
        // Lazy interposes ORC's CompileOnDemandLayer: a module's functions
        // become lazy re-export stubs, and a body is compiled the first time
        // it is *called*. Measured on a 400-function program whose entry point
        // reaches one of them, that is 773 ms of codegen down to the 28 ms the
        // program actually needs.
        //
        // Not a free win everywhere, which is why it is a choice and not the
        // rule: it costs a stub and a call-through per function, and a lazy
        // batch cannot be removed (see DropGeneration).
        enum class ECompilePolicy : std::uint8_t
        {
            Eager,
            Lazy,
        };

        // What the session is built with. A struct rather than four
        // parameters because two of them are bare integers, and `Init( 0, 2 )`
        // is not something anyone should have to decode at a call site.
        struct SessionOptions
        {

            // 0 keeps compilation synchronous.
            unsigned CompileThreads = 0;

            // A request, not a guarantee — see Init.
            ECompilePolicy Policy = ECompilePolicy::Eager;

            // What `-O<n>` asked for, meaning the same thing it means to
            // `volt build` (Ir::OptimizationLevelOf).
            //
            // *When* it is paid for depends on Policy, and the two readings are
            // the same promise about the code that ends up mattering:
            //
            //   Eager  — every module is optimised at this level as it is
            //            compiled, which is the only thing a session that
            //            compiles everything up front can do.
            //   Lazy   — nothing is. Every partition is built at O0, and only
            //            one that turns out to be called often is built again
            //            at this level (JitCompiler.cpp, InstallTiering). A
            //            program that runs one function 300k times gets it
            //            optimised; a program that touches 150 functions once
            //            each never pays for optimising any of them.
            //
            // 0 means neither: there is no second level to promote anything to,
            // so a lazy session leaves the re-optimisation layer out entirely.
            std::uint8_t OptLevel = 0;
        };

        class JitCompiler
        {

        public:

            JitCompiler ();
            ~JitCompiler ();

            JitCompiler ( const JitCompiler & )           = delete;
            JitCompiler &operator=( const JitCompiler & ) = delete;

            // Builds the LLJIT. False with OutError set on failure — an ORC
            // Error is consumed here and never allowed to escape as one.
            //
            // `Wanted.Policy` is a request, not a guarantee: Lazy needs
            // target-specific trampolines, and an architecture LLVM has none
            // for falls back to Eager rather than refusing to run. Ask
            // `Policy()` for what was actually built.
            [[nodiscard]] bool Init ( const SessionOptions &Wanted, std::string &OutError );

            // What Init settled on. Differs from what was asked for only when
            // the fallback above fired, and a caller that reports timings wants
            // to say which of the two it measured.
            [[nodiscard]] ECompilePolicy Policy () const;

            // Whether hot code will be built a second time at `OptLevel`.
            //
            // False for the same three reasons Policy() can differ from what
            // was asked for — no `-O`, no lazy stack, or no redirectable
            // symbols on this target — and for the same reason it is worth
            // asking: a run that reports its own timings is otherwise unable to
            // say which of the two shapes produced them.
            [[nodiscard]] bool Tiering () const;

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
            //
            // Always Eager, whatever the session policy. A replacement is one
            // unit's worth of code — there is next to nothing in it to defer —
            // and the two callers that open one need what Lazy cannot give:
            // `Reload` needs every new body's *own* address to store into a
            // slot, and `:bench` needs the generation back (DropGeneration).
            [[nodiscard]] bool OpenReplacement ( GenerationId &OutGen, std::string &OutError );

            // Adds a whole emission at once: the modules, and the context
            // they share. A module that defines nothing is dropped rather than
            // added — under PerUnit granularity most of them define nothing
            // (every unit whose code came from a dylib), and a materialisation
            // unit that can never be asked for anything is pure bookkeeping.
            [[nodiscard]] bool AddModules ( GenerationId Gen, Ir::OwnedModules Modules, std::string &OutError );

            // Unmaps a generation's code. Forbidden while any frame may still
            // be inside it, and refused outright for a lazy generation:
            // LLLazyJIT::addLazyIRModule takes a JITDylib and no
            // ResourceTracker, so a lazy batch lands on the dylib's default
            // tracker and removing *that* would take the dylib's other
            // contents with it. Only OpenReplacement generations are ever
            // dropped and those are never lazy, so this is a guard rather than
            // a limitation anyone meets.
            [[nodiscard]] bool DropGeneration ( GenerationId Gen, std::string &OutError );

            // Forces materialisation and yields the address.
            //
            // Under Lazy that address is the function's *stub*, not its body:
            // the body is compiled when the stub is first called. Calling it
            // is transparent; *reading the bytes there* is not, and Disassemble
            // below would decode a jump rather than a function. That is why
            // `:asm` is safe today — the REPL, its only caller, runs Eager.
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

            // Point LLJIT's IR transform layer — the identity until now — at
            // PassBuilder's pipeline for `Level`. Called by Init once the JIT
            // exists, whichever of the two it ended up building.
            //
            // `Level` is the floor rather than the rule: a module that carries
            // a tier flag overrides it, which is how a re-optimised partition
            // comes back through this same layer and gets a different pipeline.
            void InstallPipeline ( llvm::OptimizationLevel Level );

            // Assemble the lazy stack over the LLJIT Init just built: the
            // call-through manager, the partition layer, and the on-demand
            // layer that lazy modules are added to.
            //
            // These are the three objects LLLazyJIT would have owned. They are
            // built here instead because tiering has to go *between* two of
            // them — a re-optimisation layer below the partitioning sees one
            // call tree at a time, all of it code, which is the only shape it
            // accepts (JitCompiler.cpp, InstallTiering) — and LLLazyJIT hands
            // out no seam to put it in.
            //
            // False when this target has no lazy trampolines, which is Init's
            // cue to fall back to Eager.
            [[nodiscard]] bool BuildLazyStack ( std::uint8_t Tier );

            // Put ORC's ReOptimizeLayer under the partition layer, so that a
            // partition called often enough is compiled again at `Tier`.
            //
            // False when the target cannot support it, which leaves the lazy
            // stack exactly as it was: tiering is an addition to that stack,
            // never a precondition for it.
            [[nodiscard]] bool InstallTiering ( std::uint8_t Tier );

            struct Impl;
            std::unique_ptr<Impl> P;
        };

    } // namespace Jit

} // namespace Backend

} // namespace Volt
