#pragma once

// SemaType.hpp — what one compile unit knows about the types of its own
// expressions.
//
// Two representations exist on purpose. A SigType (TypeStore.hpp) is what a
// declaration wrote down and may still name a generic parameter; a SemaType is
// what an expression actually is, and is always concrete. `G<A>` is nothing
// but the pair (nominal G, [nominal A]) — an instantiation is a value, not a
// declaration, so it needs no global registration and the frozen store is
// never mutated during the parallel sema phase.
//
// `SemaType`/`SemaTypeId` themselves now live in TypeUniverse.hpp, next to the
// build-wide dictionary that mints them: a handle is canonical across every
// unit of a build, so what is left here is purely the *per-unit mapping* —
// which expression, parameter or binding site carries which type. That split
// is the whole point; see TypeUniverse.hpp's header comment.

#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/MiddleEnd/TypeSystem/BindingSite.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeUniverse.hpp"
#include "VoltMiddleEndTypeSystem_export.hpp"

#include <cstddef>
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

namespace MiddleEnd
{

    namespace TypeSystem
    {

        // Every type one compile unit inferred, plus where it applies. The
        // *mappings* below are mutated only by that unit's own pass run, so the
        // parallel sema phase needs no lock on them; the types themselves are
        // interned into the shared, internally-synchronised `TypeUniverse` this
        // is bound to.
        class VOLT_MIDDLEEND_TYPESYSTEM_EXPORT UnitTypes
        {

        public:

            // Point this unit at the build's canonical dictionary. Called once,
            // before any pass runs (Driver) or at the top of a monomorphisation
            // overlay (`ReinstantiateBody`). An unbound instance interns nothing
            // and answers no type — loudly wrong rather than quietly per-unit,
            // which is the failure mode this whole design exists to remove.
            void BindUniverse ( TypeUniverse &InUniverse )
            {
                Universe = &InUniverse;
            }

            [[nodiscard]] bool IsBound () const
            {
                return Universe != nullptr;
            }

            // Interning makes SemaTypeId equality *be* type equality — inside a
            // unit and, since the dictionary is the build's, across units too.
            //
            // `Memo` is a per-unit fast path, not storage: a shape this unit has
            // already mentioned answers without touching the universe's write
            // lock, which is what keeps a parallel TypeChecker off a shared
            // mutex on its hottest call.
            [[nodiscard]] SemaTypeId Intern ( SemaType Value )
            {
                if ( not Value.Base.IsValid() or Universe == nullptr )
                {
                    return SemaTypeId{};
                }

                std::vector<std::uint32_t> Key = TypeUniverse::KeyOf( Value );
                if ( const auto It = Memo.find( Key ); It != Memo.end() )
                {
                    return It->second;
                }

                const SemaTypeId Id = Universe->Intern( std::move( Value ) );
                Memo.emplace( std::move( Key ), Id );
                return Id;
            }

            [[nodiscard]] const SemaType &Get ( SemaTypeId Id ) const
            {
                return Universe != nullptr ? Universe->Get( Id ) : TypeUniverse::Empty();
            }

            [[nodiscard]] bool Has ( SemaTypeId Id ) const
            {
                return Universe != nullptr and Universe->Has( Id );
            }

            // The full generic signature — `G<A>`, not `G`. Every diagnostic
            // that names a type goes through here.
            [[nodiscard]] std::string Describe ( const TypeStore &Store, SemaTypeId Id ) const
            {
                return Universe != nullptr ? Universe->Describe( Store, Id ) : std::string{ "<unresolved>" };
            }

            void SetExprType ( Frontend::ExprId Expr, SemaTypeId Type )
            {
                if ( not Expr.IsValid() )
                {
                    return;
                }
                if ( Expr.Value >= OfExpr.size() )
                {
                    OfExpr.resize( static_cast<std::size_t>( Expr.Value ) + 1, SemaTypeId{} );
                }
                OfExpr[Expr.Value] = Type;
            }

            [[nodiscard]] SemaTypeId ExprType ( Frontend::ExprId Expr ) const
            {
                if ( not Expr.IsValid() or Expr.Value >= OfExpr.size() )
                {
                    return SemaTypeId{};
                }
                return OfExpr[Expr.Value];
            }

            /// An expression written inside a generic *definition* — the body of
            /// a generic type, of a mixin, of a generic method. A value of type
            /// `T` there has no SemaTypeId, and cannot: `T` only becomes a type
            /// when the definition is instantiated, which is monomorphisation,
            /// which is backend work (rules/core-ast.md). Recorded rather than
            /// recomputed, because TypeChecker is the only thing that knows where
            /// those bodies begin and end; AstInvariant reads it to tell
            /// "deferred until instantiation" apart from "the middle-end forgot".
            void MarkDeferred ( Frontend::ExprId Expr )
            {
                if ( not Expr.IsValid() )
                {
                    return;
                }
                if ( Expr.Value >= Deferred.size() )
                {
                    Deferred.resize( static_cast<std::size_t>( Expr.Value ) + 1, false );
                }
                Deferred[Expr.Value] = true;
            }

            [[nodiscard]] bool IsDeferred ( Frontend::ExprId Expr ) const
            {
                return Expr.IsValid() and Expr.Value < Deferred.size() and Deferred[Expr.Value];
            }

            void SetSiteType ( BindingSite Site, SemaTypeId Type )
            {
                SiteTypes[Site] = Type;
            }

            [[nodiscard]] SemaTypeId SiteType ( BindingSite Site ) const
            {
                const auto It = SiteTypes.find( Site );
                return It != SiteTypes.end() ? It->second : SemaTypeId{};
            }

            // How many canonical types the whole build has interned. A unit no
            // longer owns a count of its own — that is the point of a shared
            // dictionary — and every caller only ever wanted "is this handle in
            // range", which `Has` answers.
            [[nodiscard]] std::size_t Size () const
            {
                return Universe != nullptr ? Universe->Size() : 0;
            }

            // --- Frontend cache (Issue #61) -----------------------------------
            //
            // Only the mappings are persisted. The types themselves ride along
            // with `TypeStore::SerializeCache`, which replays the universe in
            // intern order so every SemaTypeId below comes back meaning exactly
            // what it meant when it was written — no remap table, the same
            // contract NominalId/SigTypeId/Symbol already keep. `Memo` is not
            // serialised either: it is a pure cache and refills itself.
            void SerializeCache ( Meta::Writer &W ) const;

            // Expects a *fresh* UnitTypes (same contract as DeserializeArena),
            // already bound to the universe the cache was replayed into.
            [[nodiscard]] bool DeserializeCache ( Meta::Reader &R );

        private:

            TypeUniverse *Universe = nullptr;
            std::unordered_map<std::vector<std::uint32_t>, SemaTypeId, U32KeyHash> Memo;
            std::vector<SemaTypeId> OfExpr; // indexed by ExprId::Value
            std::vector<bool> Deferred;     // indexed by ExprId::Value
            std::unordered_map<BindingSite, SemaTypeId, BindingSiteHash> SiteTypes;
        };

    } // namespace TypeSystem

} // namespace MiddleEnd

} // namespace Volt
