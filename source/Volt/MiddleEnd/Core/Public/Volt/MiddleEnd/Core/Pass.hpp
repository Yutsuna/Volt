#pragma once

#include "VoltMiddleEndCore_export.hpp"
#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Diagnostics/SourceManager.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Volt
{

namespace MiddleEnd
{

// Forward declarations for modules that sit higher in the DAG.
// Core is the foundation; it never includes headers from IR, TypeSystem, or Resolver.
namespace TypeSystem
{
    class TypeStore;
    struct UnitTypes;
} // namespace TypeSystem

namespace Resolver
{
    class ScopeTable;
    class InterfaceRegistry;
} // namespace Resolver

namespace IR
{
    struct UnitCallees;
    class SynthesizedFunctions;
} // namespace IR

namespace Core
{

    // Per-unit counters that passes report back through.
    struct PassStats
    {
        std::size_t JsxLowered                = 0;
        std::size_t CaseLowered               = 0;
        std::size_t DotCallsLowered           = 0;
        std::size_t AssignsLowered            = 0;
        std::size_t IndexesLowered            = 0;
        std::size_t InterpsLowered            = 0;
        std::size_t ResidualSugarNodes        = 0;
        std::size_t UntypedValueExprs         = 0;
        std::size_t MacrosExpanded            = 0;
        std::size_t PipelinesLowered          = 0;
        std::size_t MagicsExpanded            = 0;
        std::size_t ScopesResolved            = 0;
        std::size_t UnresolvedIdentifiers     = 0;

        std::size_t RaiiLocals                = 0;
        std::size_t RaiiTemporaries           = 0;
        std::size_t RaiiOwnedCreated          = 0;
        std::size_t RaiiMoves                 = 0;
        std::size_t RaiiFinalizes             = 0;
        std::size_t RaiiExplicitEscapes       = 0;
        std::size_t RaiiOwnedWithoutCleanup   = 0;
        std::size_t RaiiCleanupPaths          = 0;
        std::size_t RaiiNestedExpressionExits = 0;
        std::size_t RaiiUnsupportedExits      = 0;
        std::size_t RaiiRuntimeOwnershipFlags = 0;

        void Merge ( const PassStats &Other )
        {
            std::array<std::size_t, Meta::FieldCount<PassStats>()> Values{};

            std::size_t Index = 0;
            Meta::ForEachField( Other, [&] ( std::string_view, const std::size_t &Value ) { Values[Index++] = Value; } );
            Index = 0;
            Meta::ForEachField( *this, [&] ( std::string_view, std::size_t &Value ) { Value += Values[Index++]; } );
        }
    };

    // Everything a pass is allowed to touch for one source file.
    struct PassContext
    {
        Frontend::AstContext &Ast;
        const TypeSystem::TypeStore &Types;
        TypeSystem::UnitTypes &Values;
        Resolver::ScopeTable &Scopes;
        Volt::Core::DiagEngine::Bag &Diags;
        PassStats &Stats;
        const Resolver::InterfaceRegistry *Globals = nullptr;
        const Volt::Core::SourceManager *Sources   = nullptr;
        IR::UnitCallees *Callees                   = nullptr;
        IR::SynthesizedFunctions &Synth;
    };

    // A pass is a pure function over a PassContext.
    using PassFn = void ( * )( PassContext & );

    enum class EPassKind : std::uint8_t
    {
        Analysis, // reads the AST, reports diagnostics
        Lowering, // rewrites the AST in place
    };

    struct PassInfo
    {
        std::string_view Name;
        int Order      = 0;
        EPassKind Kind = EPassKind::Analysis;
        PassFn Run     = nullptr;
    };

} // namespace Core

// Forward declarations of pass functions in their target namespaces (DAG order).
namespace Lowering
{
    void FunctionalLowering ( Core::PassContext &Context );
    void PipelineLowering ( Core::PassContext &Context );
    void JsxLowering ( Core::PassContext &Context );
    void CaseLowering ( Core::PassContext &Context );
    void DotCallLowering ( Core::PassContext &Context );
    void AssignLowering ( Core::PassContext &Context );
    void IndexLowering ( Core::PassContext &Context );
    void InterpLowering ( Core::PassContext &Context );
} // namespace Lowering

namespace Resolver
{
    void ScopeResolver ( Core::PassContext &Context );
} // namespace Resolver

namespace ConstEval
{
    void MacroExpansion ( Core::PassContext &Context );
    void MagicExpansion ( Core::PassContext &Context );
} // namespace ConstEval

namespace Analysis
{
    void TypeChecker ( Core::PassContext &Context );
    void UnusedChecker ( Core::PassContext &Context );
    void AstInvariant ( Core::PassContext &Context );
} // namespace Analysis

namespace Core
{

    // The manifest passes, sorted ascending by Order (built once, cached).
    [[nodiscard]] VOLT_MIDDLEEND_CORE_EXPORT std::span<const PassInfo> PassRegistry ();

    // Run every registered pass over Context, in Order. Returns the number of passes executed.
    VOLT_MIDDLEEND_CORE_EXPORT std::size_t RunPasses ( PassContext &Context );

    // Same, restricted to the passes of one Kind (manifest order preserved).
    VOLT_MIDDLEEND_CORE_EXPORT std::size_t RunPasses ( PassContext &Context, EPassKind Only );

    // Same, restricted to the half-open Order window `[Begin, End)`.
    VOLT_MIDDLEEND_CORE_EXPORT std::size_t RunPasses ( PassContext &Context, int OrderBegin, int OrderEnd );

    // The Order at which the per-unit sema run can be cut in two.
    [[nodiscard]] VOLT_MIDDLEEND_CORE_EXPORT int LoweredSeamOrder ();

} // namespace Core

} // namespace MiddleEnd

} // namespace Volt
