#pragma once

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/MiddleEnd/Analysis/AnalysisTypes.hpp"
#include "Volt/MiddleEnd/IR/CalleeMap.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndAnalysis_export.hpp"

#include <span>

// Can calling this member leave the unwind transport armed?
//
// Volt's tier 1 transport is personality-less: `raise` publishes into
// thread-local state and returns, so a caller learns what happened only by
// *checking* that state after the call (BackendCore/UnwindTransport.hpp). Every
// backend therefore follows every Volt call with two thread-local loads, a
// compare, an `or`, a branch — and a split of the basic block the call sat in.
//
// That is a per-call-site cost paid by every body in the program, including the
// overwhelming majority that cannot raise at all. This analysis is what buys it
// back: a member whose body, and whose whole reachable callee set, can neither
// `raise` nor take a non-local `break` needs no check after it, at any
// optimisation level and with no help from the inliner.
//
// It is derived, not annotated — `rules/zero-hardcode.md`'s list stays closed,
// and "can this raise" is computable from the bodies the compiler already has.
namespace Volt::MiddleEnd::Analysis::Unwind
{

// Stamps `Member::bCanUnwind` across the whole store.
//
// **Where this must run**: its own serial seam, *after* the parallel
// `TypeChecker` wave, and before any backend reads a `Member`. Later than the
// RAII fixpoints on purpose. Those run before typing and so have no
// `UnitCallees` to read — they fall back to a by-spelling name index, an
// approximation this one can mostly avoid, because by the time typing is done
// every call site has a resolved callee recorded against it. Running here is
// also what makes `Member` addresses safe to hold: the store stopped growing at
// the interface seam, long before.
//
// `Units` and `Callees` are both indexed by unit ordinal, exactly like
// `Member::Unit`, and must be the same length. A null AST entry is a cache-hit
// stdlib slot: its members carry the bit *from the cache* (`Member` is a
// reflected aggregate, so it serializes with everything else), and its bodies
// are deliberately not re-analysed.
//
// The fixpoint is monotone upward — a body the analysis can see starts `false`
// and only ever flips to `true` — so it terminates, and mutual recursion
// resolves the useful way round: a cycle that raises nothing stays `false`
// rather than poisoning itself. Everything it cannot see stays `true`: an
// `@[External]` Volt seam, an `abstract def` with no implementation in the
// build, an indirect or dynamically dispatched callee. The arbitration is not
// symmetric — a needless check costs a branch, a missing one resumes past a
// raise — so nothing here flips a bit to `false` without a body to prove it.
VOLT_MIDDLEEND_ANALYSIS_EXPORT void InferUnwindFreedom ( std::span<const Frontend::AstContext *const> Units,
                                                         std::span<const IR::UnitCallees *const> Callees,
                                                         TypeStore &Store );

} // namespace Volt::MiddleEnd::Analysis::Unwind
