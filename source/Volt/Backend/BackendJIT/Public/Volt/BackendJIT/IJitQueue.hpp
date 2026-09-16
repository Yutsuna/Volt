#pragma once

// IJitQueue.hpp — the abstract interface for JIT compilation & execution queues.
//
// Decouples the high-level execution backend (JitBackend) from the underlying
// execution engine (LLVM ORC, or a future direct machine-code emitter).
//
// JitBackend manages:
//   - Process execution, command line arguments, exit codes
//   - Unwind exception buffer storage & transport accessors
//   - Indirection slot tables (@volt.fn.*) and atomic store patching
//   - Dynamic dispatch mutable vtables and vtable patching
//   - Reload verification (signature equality, type layout stability)
//   - REPL evaluation & incremental session state
//
// IJitQueue manages:
//   - Target architecture specification (triple, data layout)
//   - Generation allocation, replacement dylibs, resource tracking
//   - Dynamic library & process symbol loading
//   - Compilation of units (AST / BackendInput -> machine executable memory)
//   - Symbol address resolution (Lookup, LookupIn)
//   - Disassembly & IR rendering

#include "BackendJIT_export.hpp"
#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/TargetBackend.hpp"
#include "Volt/BackendJIT/JitBackend.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Backend
{

    namespace Jit
    {

        using GenerationId = std::uint32_t;

        enum class ECompilePolicy : std::uint8_t
        {
            Eager,
            Lazy,
        };

        struct SessionOptions
        {
            unsigned CompileThreads = 0;
            ECompilePolicy Policy   = ECompilePolicy::Eager;
            std::uint8_t OptLevel   = 0;
        };

        // Metadata collected during unit emission for reload and vtable validation
        struct CompiledUnitMeta
        {
            struct SymbolDef
            {
                std::string Name;
                std::string Signature;
                [[nodiscard]] bool operator==( const SymbolDef & ) const = default;
            };

            struct TypeShape
            {
                std::string Name;
                std::size_t Size                                         = 0;
                std::size_t Alignment                                    = 1;
                [[nodiscard]] bool operator==( const TypeShape & ) const = default;
            };

            struct VTableEntry
            {
                std::string VTable;
                std::uint32_t Slot = 0;
                std::string Function;
                [[nodiscard]] bool operator==( const VTableEntry & ) const = default;
            };

            std::vector<SymbolDef> Symbols;
            std::vector<TypeShape> Shapes;
            std::vector<VTableEntry> VTables;
            std::vector<std::string> DefinedSymbols;
        };

        class BACKENDJIT_EXPORT IJitQueue
        {

        public:

            virtual ~IJitQueue () = default;

            // Target machine information
            [[nodiscard]] virtual std::string TargetTriple () const     = 0;
            [[nodiscard]] virtual std::string DataLayoutString () const = 0;

            // Session configuration
            [[nodiscard]] virtual bool Init ( const SessionOptions &Wanted, std::string &OutError ) = 0;
            [[nodiscard]] virtual ECompilePolicy Policy () const                                    = 0;
            [[nodiscard]] virtual bool Tiering () const                                             = 0;

            // Linking & symbol resolution
            [[nodiscard]] virtual bool AddDylib ( std::string_view Path, std::string &OutError )                          = 0;
            [[nodiscard]] virtual bool AddProcessSymbols ( std::string &OutError )                                        = 0;
            [[nodiscard]] virtual bool Lookup ( std::string_view Symbol, std::uintptr_t &OutAddr, std::string &OutError ) = 0;
            [[nodiscard]] virtual bool
            LookupIn ( GenerationId Gen, std::string_view Symbol, std::uintptr_t &OutAddr, std::string &OutError ) = 0;

            // Generation tracking
            [[nodiscard]] virtual GenerationId OpenGeneration ()                                       = 0;
            [[nodiscard]] virtual bool OpenReplacement ( GenerationId &OutGen, std::string &OutError ) = 0;
            [[nodiscard]] virtual bool DropGeneration ( GenerationId Gen, std::string &OutError )      = 0;
            [[nodiscard]] virtual std::size_t LiveGenerations () const                                 = 0;

            // Initial program compilation (Begin / EmitUnit / Finalize)
            virtual void Begin ( const BackendInput &Input, const JitOptions &Options )                    = 0;
            [[nodiscard]] virtual EEmitStatus EmitUnit ( const UnitView &Unit, CompiledUnitMeta &OutMeta ) = 0;
            [[nodiscard]] virtual EmitResult Finalize ( GenerationId Gen, CompiledUnitMeta &OutMeta )      = 0;
            [[nodiscard]] virtual std::size_t UnwindStorageSize () const                                   = 0;

            // Incremental compilation
            using SymbolPredicate = std::function<bool( std::string_view )>;

            // Hot reload staging: prepare replacement to inspect shapes & symbols, then commit or discard.
            [[nodiscard]] virtual bool PrepareReplacement ( const BackendInput &Build,
                                                            const UnitView &Unit,
                                                            const SymbolPredicate &IsAlreadyDefined,
                                                            const SymbolPredicate &HasIndirectionSlot,
                                                            CompiledUnitMeta &OutMeta,
                                                            std::string &OutError )                  = 0;
            [[nodiscard]] virtual bool CommitReplacement ( GenerationId Gen, std::string &OutError ) = 0;
            virtual void DiscardReplacement ()                                                       = 0;

            [[nodiscard]] virtual bool CompileEvalUnit ( const BackendInput &Build,
                                                         const UnitView &Unit,
                                                         GenerationId &OutGen,
                                                         bool &OutRedefines,
                                                         const SymbolPredicate &IsAlreadyDefined,
                                                         const SymbolPredicate &HasIndirectionSlot,
                                                         CompiledUnitMeta &OutMeta,
                                                         std::size_t BootUnwindStorage,
                                                         std::string &OutError ) = 0;

            [[nodiscard]] virtual bool CompileBenchUnit ( const BackendInput &Build,
                                                          const UnitView &Unit,
                                                          GenerationId &OutGen,
                                                          const SymbolPredicate &IsAlreadyDefined,
                                                          const SymbolPredicate &HasIndirectionSlot,
                                                          std::string &OutError ) = 0;

            [[nodiscard]] virtual bool
            ProbeUnit ( const BackendInput &Build, const UnitView &Unit, std::string *OutIr, std::string &OutError ) = 0;

            // Introspection
            virtual void RecordIr ( bool bEnable )            = 0;
            [[nodiscard]] virtual std::string LastIr () const = 0;
            [[nodiscard]] virtual std::string
            Disassemble ( std::uintptr_t Address, std::size_t MaxBytes, std::string &OutError ) const = 0;
        };

        // Factory function creating the standard LLVM ORC JIT queue.
        [[nodiscard]] BACKENDJIT_EXPORT std::unique_ptr<IJitQueue> CreateOrcJitQueue ();

    } // namespace Jit

} // namespace Backend

} // namespace Volt
