#pragma once

// EmitterServices.hpp — the bundle every emitter is handed, and the two module
// -wide key types the caches are keyed on.
//
// This replaces the old `LlvmBackend::State` god object: rather than one class
// declaring ~90 emission methods and owning every cache, each concern is its
// own small service and this struct is how they reach each other. It is
// deliberately a bag of *pointers*, not an owner — `State` owns, this refers —
// so passing it costs nothing and no service outlives the state that made it.
//
// The discipline that keeps it from becoming a second god object: a service
// reads what it needs from here and nothing else may be added that is not a
// service, an option, or a build-wide cache. Per-function state lives in
// FunctionFrame, never here.

#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"
#include "Volt/BackendCore/InstanceLayout.hpp"
#include "Volt/BackendCore/LayoutEngine.hpp"
#include "Volt/BackendLlvmIr/IrGenerator.hpp"

#include "Core/LlvmFwd.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class AbiVerifier;
        class ClosureLowering;
        class ExceptionLowering;
        class FunctionRegistry;
        class ModuleContext;
        class MonoDriver;
        class SignatureBuilder;
        class TypeMapper;
        class VTableRegistry;

        // Key for top-level module global variables: combines the unit's
        // ordinal and the BindingSite so that AST node indices from different
        // units never collide in ModuleGlobals.
        struct UnitGlobalKey
        {

            std::uint32_t Ordinal = 0;
            MiddleEnd::Resolver::BindingSite Site;

            bool operator==( const UnitGlobalKey &Other ) const
            {
                return Ordinal == Other.Ordinal and Site == Other.Site;
            }
        };

        struct UnitGlobalKeyHash
        {

            [[nodiscard]] std::size_t operator()( const UnitGlobalKey &Key ) const
            {
                const std::size_t H1 = std::hash<std::uint32_t>{}( Key.Ordinal );
                const std::size_t H2 =
                    std::visit( [] ( const auto &Id ) { return std::hash<std::uint32_t>{}( Id.Value ); }, Key.Site );
                return H1 ^ ( H2 << 1U );
            }
        };

        // Same shape as UnitGlobalKey, for a DeclId instead of a BindingSite:
        // a synthesized function (ClosureLifting) has no mangled symbol — it is
        // never looked up by name — so it is keyed by where it lives instead,
        // the same reason a global needs the unit's ordinal folded in.
        struct UnitDeclKey
        {

            std::uint32_t Ordinal = 0;
            Frontend::DeclId Decl;

            bool operator==( const UnitDeclKey &Other ) const
            {
                return Ordinal == Other.Ordinal and Decl == Other.Decl;
            }
        };

        struct UnitDeclKeyHash
        {

            [[nodiscard]] std::size_t operator()( const UnitDeclKey &Key ) const
            {
                const std::size_t H1 = std::hash<std::uint32_t>{}( Key.Ordinal );
                const std::size_t H2 = std::hash<std::uint32_t>{}( Key.Decl.Value );
                return H1 ^ ( H2 << 1U );
            }
        };

        // Top-level module globals: keyed by (Unit.Ordinal, BindingSite) so
        // variables declared at top level retain their storage across statements
        // and functions without key collisions across units.
        using ModuleGlobalMap = std::unordered_map<UnitGlobalKey, llvm::GlobalVariable *, UnitGlobalKeyHash>;

        // A unit's own UnitView::Synth entries, once declared.
        using SynthesizedFnMap = std::unordered_map<UnitDeclKey, llvm::Function *, UnitDeclKeyHash>;

        struct EmitterServices
        {

            // --- The middle-end's output, read-only except for layouts -------
            const BackendInput *Build            = nullptr;
            const Ir::IrOptions *Options         = nullptr;
            std::optional<LayoutEngine> *Layouts = nullptr;
            InstanceLayouts *Instances           = nullptr;

            // --- Services ----------------------------------------------------
            DiagnosticSink *Diag          = nullptr;
            ModuleContext *Ctx            = nullptr;
            TypeMapper *Types             = nullptr;
            AbiVerifier *Abi              = nullptr;
            SignatureBuilder *Signatures  = nullptr;
            FunctionRegistry *Functions   = nullptr;
            ExceptionLowering *Exceptions = nullptr;
            ClosureLowering *Closures     = nullptr;
            MonoDriver *Mono              = nullptr;
            VTableRegistry *VTables       = nullptr;

            // --- Build-wide caches that belong to no single service ----------
            ModuleGlobalMap *ModuleGlobals   = nullptr;
            SynthesizedFnMap *SynthesizedFns = nullptr;

            // Indexed by unit ordinal: 1 when that unit's code lives in an
            // artifact this build links or loads rather than emits. Resolved
            // once in Begin, because the answer involves a walk of the whole
            // TypeStore and every function declaration would otherwise ask it
            // again.
            const std::vector<std::uint8_t> *PrecompiledUnits = nullptr;

            // Under PerUnit granularity, the ordinal of the unit whose module
            // is being written right now — the one module allowed to *define*
            // that unit's bodies. NoUnitOrdinal while the shared module is
            // being written: the declarations Begin sweeps out, and everything
            // Finish caps the emission with. Whole granularity leaves it alone
            // and nothing reads it.
            static constexpr std::uint32_t NoUnitOrdinal = 0xFFFFFFFFU;
            std::uint32_t CurrentUnit                    = NoUnitOrdinal;
        };

        // One module per unit, rather than one for the whole build. Read often
        // enough — and through a pointer often enough — to be worth naming.
        [[nodiscard]] inline bool PerUnitModules ( const EmitterServices &Services )
        {
            return Services.Options != nullptr and Services.Options->Granularity == Ir::EModuleGranularity::PerUnit;
        }

        // Calls reach their callee through `@volt.fn.<sym>` rather than by
        // relocation. Named here beside PerUnitModules because it has stopped
        // being one emitter's private question: it is the single announcement
        // that this build will be reloaded, and everything that has to stay
        // movable afterwards — a call site, a vtable's pointers — reads it.
        [[nodiscard]] inline bool IndirectLinkage ( const EmitterServices &Services )
        {
            return Services.Options != nullptr and Services.Options->Linkage == Ir::ELinkage::Indirect;
        }

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
