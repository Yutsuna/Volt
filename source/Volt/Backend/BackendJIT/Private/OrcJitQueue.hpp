#pragma once

// OrcJitQueue.hpp — LLVM ORC implementation of IJitQueue.
//
// Manages LLJIT, LLLazyJIT, IRPartitionLayer, ReOptimizeLayer, IrGenerator,
// and the PassBuilder pipeline for Volt's JIT.

#include "Volt/BackendJIT/IJitQueue.hpp"

#include <memory>

namespace Volt
{

namespace Backend
{

    namespace Jit
    {

        class OrcJitQueue final : public IJitQueue
        {

        public:

            OrcJitQueue ();
            ~OrcJitQueue () override;

            OrcJitQueue ( const OrcJitQueue & )           = delete;
            OrcJitQueue &operator=( const OrcJitQueue & ) = delete;
            OrcJitQueue ( OrcJitQueue && ) noexcept;
            OrcJitQueue &operator=( OrcJitQueue && ) noexcept;

            [[nodiscard]] std::string TargetTriple () const override;
            [[nodiscard]] std::string DataLayoutString () const override;

            [[nodiscard]] bool Init ( const SessionOptions &Wanted, std::string &OutError ) override;
            [[nodiscard]] ECompilePolicy Policy () const override;
            [[nodiscard]] bool Tiering () const override;

            [[nodiscard]] bool AddDylib ( std::string_view Path, std::string &OutError ) override;
            [[nodiscard]] bool AddProcessSymbols ( std::string &OutError ) override;
            [[nodiscard]] bool Lookup ( std::string_view Symbol, std::uintptr_t &OutAddr, std::string &OutError ) override;
            [[nodiscard]] bool
            LookupIn ( GenerationId Gen, std::string_view Symbol, std::uintptr_t &OutAddr, std::string &OutError ) override;

            [[nodiscard]] GenerationId OpenGeneration () override;
            [[nodiscard]] bool OpenReplacement ( GenerationId &OutGen, std::string &OutError ) override;
            [[nodiscard]] bool DropGeneration ( GenerationId Gen, std::string &OutError ) override;
            [[nodiscard]] std::size_t LiveGenerations () const override;

            void Begin ( const BackendInput &Input, const JitOptions &Options ) override;
            [[nodiscard]] EEmitStatus EmitUnit ( const UnitView &Unit, CompiledUnitMeta &OutMeta ) override;
            [[nodiscard]] EmitResult Finalize ( GenerationId Gen, CompiledUnitMeta &OutMeta ) override;
            [[nodiscard]] std::size_t UnwindStorageSize () const override;

            [[nodiscard]] bool PrepareReplacement ( const BackendInput &Build,
                                                    const UnitView &Unit,
                                                    const SymbolPredicate &IsAlreadyDefined,
                                                    const SymbolPredicate &HasIndirectionSlot,
                                                    CompiledUnitMeta &OutMeta,
                                                    std::string &OutError ) override;
            [[nodiscard]] bool CommitReplacement ( GenerationId Gen, std::string &OutError ) override;
            void DiscardReplacement () override;

            [[nodiscard]] bool CompileEvalUnit ( const BackendInput &Build,
                                                 const UnitView &Unit,
                                                 GenerationId &OutGen,
                                                 bool &OutRedefines,
                                                 const SymbolPredicate &IsAlreadyDefined,
                                                 const SymbolPredicate &HasIndirectionSlot,
                                                 CompiledUnitMeta &OutMeta,
                                                 std::size_t BootUnwindStorage,
                                                 std::string &OutError ) override;

            [[nodiscard]] bool CompileBenchUnit ( const BackendInput &Build,
                                                  const UnitView &Unit,
                                                  GenerationId &OutGen,
                                                  const SymbolPredicate &IsAlreadyDefined,
                                                  const SymbolPredicate &HasIndirectionSlot,
                                                  std::string &OutError ) override;

            [[nodiscard]] bool
            ProbeUnit ( const BackendInput &Build, const UnitView &Unit, std::string *OutIr, std::string &OutError ) override;

            void RecordIr ( bool bEnable ) override;
            [[nodiscard]] std::string LastIr () const override;
            [[nodiscard]] std::string
            Disassemble ( std::uintptr_t Address, std::size_t MaxBytes, std::string &OutError ) const override;

        private:

            struct Impl;
            std::unique_ptr<Impl> P;
        };

    } // namespace Jit

} // namespace Backend

} // namespace Volt
