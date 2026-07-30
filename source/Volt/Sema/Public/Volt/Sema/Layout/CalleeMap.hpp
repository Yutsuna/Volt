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

#include "Sema_export.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Volt
{

namespace Meta
{
    class Writer;
    class Reader;
} // namespace Meta

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
        // `T.new( … )` — the callee is the type's initializer, and the call
        // *constructs*: the receiver expression names a type rather than a
        // value, storage for one instance has to come from the call itself,
        // and the result is that storage, not the initializer's own (Void)
        // result. Recorded here because MemberType is where the decision is
        // taken (it is what substitutes the receiver back into `Result`);
        // re-deriving it in a backend would mean recognising the spelling
        // "new", which is a Volt name no backend may know.
        bool bConstructs = false;
        // The callee is a *value*, not a symbol: `f( x )` where `f` holds a
        // callable. The call goes through the `{ code, env }` pair rather
        // than to a mangled name, and the signature above came from the
        // receiver's type arguments rather than from `Decl->Params` — a
        // callable's arity lives in its type, which no fixed declaration
        // could express.
        //
        // Recorded here for the same reason bConstructs is: the decision
        // belongs to the resolver, which is the one place that knows the
        // receiver is the type claiming the FuncType node kind. A backend
        // re-deriving it would have to identify that type itself, and every
        // target would then carry the same identification
        // (rules/zero-hardcode.md).
        bool bIndirect = false;
    };

    // Every callee one compile unit resolved, keyed by the *callee
    // expression's* ExprId (the `Member` / `Binary` / `Unary` / `Identifier`
    // node, not the wrapping `Call`).
    class SEMA_EXPORT UnitCallees
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

        // --- Frontend cache (Issue #61) -----------------------------------
        //
        // CalleeEntry::Decl is a raw `const Member*` into the build-wide
        // TypeStore — not serialisable as a value. Every other field is a
        // plain value (SemaTypeId, SmallVec, bool), so only Decl gets the
        // plan's two-phase fixup: SerializeCache writes (Unit, DeclId)
        // instead of the pointer; DeserializeCache leaves Decl null and
        // records the key; FixupDecls re-resolves every pointer in one pass
        // once the TypeStore it points into has itself been loaded.
        void SerializeCache ( Meta::Writer &W ) const;
        [[nodiscard]] bool DeserializeCache ( Meta::Reader &R );

        // Must run after the TypeStore this map's Decls point into has
        // finished its own DeserializeCache — TypeStore::FindMemberByUnitDecl
        // reads its (by-then-frozen) Types/Functions.
        void FixupDecls ( const TypeStore &Store );

    private:

        std::unordered_map<std::uint32_t, CalleeEntry> Entries;

        // Populated by DeserializeCache, drained by FixupDecls: for each
        // Entries key with a resolved Decl, the (Unit, DeclId) that
        // identifies it across the whole build.
        std::vector<std::pair<std::uint32_t, std::pair<std::uint32_t, Frontend::DeclId>>> PendingDecls;
    };

} // namespace Sema

} // namespace Volt
