#pragma once

// SynthesizedFunctions.hpp — the per-unit side table a lowering pass uses to
// hand codegen a function it created itself (ClosureLifting, per
// .agents/PLAN_CLOSURE_LOWERING.md's Phase 3), without registering it in the
// cross-unit TypeStore.
//
// Why this exists rather than an ordinary TypeStore member: DeclareAll/
// DefineAll (BackendLLVM/.../LlvmEmitter.cpp) drive codegen off
// TypeStore::FreeFunctions()/Members, which is populated once, serially, at
// the Driver's seam (ResolveUnitSignatures) — then read concurrently,
// lock-free, by every unit's TypeChecker running in parallel
// (Driver::CompileRefs's ForEachUnitParallel). A pass running *inside*
// TypeChecker cannot safely append to that registry. Nothing outside a unit
// ever needs to name a synthesized function by symbol anyway (it is only
// ever reached through a `FuncAddr` node naming its DeclId directly), so it
// needs no TypeStore membership at all — just enough for this unit's own
// codegen to declare and define it.
//
// Thread-safe to populate for the reason ordinary UnitTypes/ScopeTable
// writes are: one instance per unit, written only by that unit's own
// single-threaded pass run.

#include "Sema_export.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"

#include <span>
#include <vector>

namespace Volt
{

namespace Sema
{

    // One function a lowering pass synthesized into the unit's own AstContext
    // (an ordinary Frontend::Method Decl, added to Ast.TopDecls the same way
    // Phase 0's spike proved) — Result/Params are already-resolved
    // SemaTypeIds, since the pass that creates one only ever runs after the
    // type information it needs has settled (core-ast.md's "typed after
    // substitution" contract, not "typed before the seam").
    struct SynthesizedFunction
    {

        Frontend::DeclId Decl;
        SemaTypeId Result;
        Core::SmallVec<SemaTypeId, 4> Params;
    };

    class SEMA_EXPORT SynthesizedFunctions
    {

    public:

        void Add ( SynthesizedFunction Fn )
        {
            Entries.push_back( std::move( Fn ) );
        }

        [[nodiscard]] std::span<const SynthesizedFunction> All () const
        {
            return Entries;
        }

    private:

        std::vector<SynthesizedFunction> Entries;
    };

} // namespace Sema

} // namespace Volt
