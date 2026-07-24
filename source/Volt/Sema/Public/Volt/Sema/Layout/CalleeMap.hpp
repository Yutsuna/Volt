#pragma once

// CalleeMap.hpp — the per-unit callee resolutions a backend consumes.
//
// TypeChecker resolves every member-ish expression (`Member`, `Binary`,
// `Unary`, free-function `Identifier`) inside its own pass-local state and
// used to drop that state on the floor when the pass returned. A backend
// reads the protocol of rules/core-ast.md off this map instead:
//
//   Has( Id ), Decl != nullptr  ->  emit a call to that method
//   otherwise                   ->  a machine instruction, selected from the
//                                   receiver's Primitive{ Spelling, Bits }
//
// One snapshot per unit, written once at the end of TypeChecker and read-only
// afterwards — the same lock-free discipline as UnitTypes.

#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstdint>
#include <unordered_map>
#include <utility>

namespace Volt
{

namespace Sema
{

    // One resolved callee, with its already-instantiated signature. `Decl`
    // points into the frozen TypeStore (never reallocated after the serial
    // binder seam), so it stays valid for as long as the build's store does.
    struct CalleeEntry
    {

        const Member *Decl = nullptr;
        SemaTypeId Result;
        Core::SmallVec<SemaTypeId, 4> Params;
        // The instantiated `&block` slot, out of Params because it binds
        // through the call's trailing `do ... end`, never positionally.
        SemaTypeId BlockParam;
        // The parameter space the signature was instantiated against — the
        // owner's arguments first, then one slot per method generic — plus
        // the receiver they were resolved on. This is what monomorphisation
        // substitutes into a generic body at codegen time.
        Core::SmallVec<SemaTypeId, 2> Bindings;
        SemaTypeId Receiver;
    };

    // Every callee one compile unit resolved, keyed by the *callee
    // expression's* ExprId (the `Member` / `Binary` / `Unary` / `Identifier`
    // node, not the wrapping `Call`).
    class UnitCallees
    {

    public:

        void Set ( Frontend::ExprId Expr, CalleeEntry Entry )
        {
            if ( Expr.IsValid() )
            {
                Entries[Expr.Value] = std::move( Entry );
            }
        }

        [[nodiscard]] const CalleeEntry *Get ( Frontend::ExprId Expr ) const
        {
            if ( not Expr.IsValid() )
            {
                return nullptr;
            }
            const auto It = Entries.find( Expr.Value );
            return It != Entries.end() ? &It->second : nullptr;
        }

        [[nodiscard]] bool Has ( Frontend::ExprId Expr ) const
        {
            return Get( Expr ) != nullptr;
        }

        [[nodiscard]] std::size_t Size () const
        {
            return Entries.size();
        }

    private:

        std::unordered_map<std::uint32_t, CalleeEntry> Entries;
    };

} // namespace Sema

} // namespace Volt
