#pragma once

#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstdint>
#include <string_view>

// The vocabulary of Volt's RAII model, at the level of *types* only.
//
// Nothing here knows about control flow, statements, lifetimes or cleanup
// boundaries — those live in `Passes/TypeChecker/Raii/`. This header answers
// exactly three questions, all of them about a type:
//
//   - does a value of this type need finalizing at all?
//   - if it is a sequence, what does it hold?
//   - what ownership state does a value carry?
//
// Keeping them here is what lets the Driver seam (`FinalizeSynthesis`, which
// runs before signatures resolve) and the in-`TypeChecker` sweeps share one
// answer instead of the two drifting copies this replaced
// (`FinalizeLowering.cpp`'s `IsFinalizeCandidateType` against a `SemaTypeId`
// and `TypeBinder.cpp`'s `IsFinalizeCandidateNominal` against a `NominalId`).
namespace Volt::Sema::Raii
{

// The RAII-style destructor spelling. A *member* spelling, not a type name,
// so `rules/zero-hardcode.md`'s guardrail — which greps for Volt *type*
// identifiers — does not apply: which member a type declares is Volt's to
// choose and the compiler only ever looks it up by this name, never assumes
// anything about its body.
//
// This is the single definition. It used to exist twice, verbatim, in
// `MemberResolver.hpp` and `TypeBinder.cpp`, because the two sites sat on
// opposite sides of a module-internal directory boundary.
constexpr std::string_view FinalizeName = "finalize";

// What a value expression's result *is*, with respect to the buffer(s) behind
// it. The whole RAII model is a discipline over this one enum:
//
//   Owned    — this expression produced a fresh value; someone must finalize
//              it exactly once.
//   Borrowed — this expression read an existing place; finalizing it here
//              would free a buffer its real owner still holds.
//   Moved    — this value *was* Owned, and its ownership has since been
//              transferred (bound to a name that outlives the region,
//              returned, stored into a field). Its original owner must no
//              longer finalize it.
//
// `Borrowed` is the safe default and the initial state for anything not
// *proven* to be one of the other two: a missed `Owned` costs a leak, which
// is a gap this compiler already accepts and measures; a wrong `Owned` costs
// a double free, which it does not.
enum class EOwnership : std::uint8_t
{
    Borrowed,
    Owned,
    Moved,
};

// Aggregate-layout and declaring `FinalizeName`, against a bare `NominalId`.
//
// A field never needs substitution here, since a generic field's own nominal
// never attaches a Layout in the first place — so this returns false for it
// automatically, which is the wall `.agents/CASCADE_FINALIZE.md` item 2
// documents.
[[nodiscard]] bool IsFinalizeCandidateNominal ( const TypeStore &Store, NominalId Id );

// The same candidacy against a receiver's own (possibly generic-argument
// bearing) `SemaTypeId`, so an inherited `finalize` and a generic
// instantiation both resolve. `Values` is non-const because `LookupMemberOn`
// instantiates the member signature it finds.
[[nodiscard]] bool IsFinalizeCandidateType ( const TypeStore &Store, UnitTypes &Values, SemaTypeId Type );

// The element type a *sequence* receiver cascades into, or an invalid id when
// the receiver is not a sequence, or holds nothing that needs finalizing.
//
// "Has a generic argument that declares finalize" is deliberately NOT the
// test — see the implementation for the two ways that misfires.
[[nodiscard]] SemaTypeId ContainerElementType ( const TypeStore &Store, UnitTypes &Values, SemaTypeId Type );

} // namespace Volt::Sema::Raii
