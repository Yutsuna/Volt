#pragma once

// SemaType.hpp — the type of an *expression*, as a value, per compile unit.
//
// Two representations exist on purpose. A SigType (TypeStore.hpp) is what a
// declaration wrote down and may still name a generic parameter; a SemaType is
// what an expression actually is, and is always concrete. `Array<Int32>` is
// nothing but the pair (nominal Array, [nominal Int32]) — an instantiation is
// a value, not a declaration, so it needs no global registration and the
// frozen store is never mutated during the parallel sema phase.

#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/MiddleEnd/TypeSystem/BindingSite.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndTypeSystem_export.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
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
        struct SemaTypeTag
        {
        };

        using SemaTypeId = ::Volt::Core::TypedId<SemaTypeTag>;

        // An expression's type: a nominal plus its (already concrete) arguments.
        // An invalid Base means "not inferred", which is a normal outcome and
        // never on its own a diagnostic.
        struct SemaType
        {

            NominalId Base;
            ::Volt::Core::SmallVec<SemaTypeId, 2> Args;
        };

        // Every type one compile unit inferred. Mutated only by that unit's own
        // pass run, so the parallel sema phase needs no lock: unlike the store,
        // this is per-file state living on the CompileUnit.
        class VOLT_MIDDLEEND_TYPESYSTEM_EXPORT UnitTypes
        {

        public:

            // Interning makes SemaTypeId equality *be* type equality inside a
            // unit; across units, NominalIds are the global currency.
            [[nodiscard]] SemaTypeId Intern ( SemaType Value )
            {
                if ( not Value.Base.IsValid() )
                {
                    return SemaTypeId{};
                }

                std::vector<std::uint32_t> Key;
                Key.reserve( 1 + Value.Args.Size() );
                Key.push_back( Value.Base.Value );
                for ( const SemaTypeId Arg : Value.Args )
                {
                    Key.push_back( Arg.Value );
                }

                if ( const auto It = Dedup.find( Key ); It != Dedup.end() )
                {
                    return It->second;
                }

                const SemaTypeId Id = Types.Add( std::move( Value ) );
                Dedup.emplace( std::move( Key ), Id );
                return Id;
            }

            [[nodiscard]] const SemaType &Get ( SemaTypeId Id ) const
            {
                return Types.Get( Id );
            }

            [[nodiscard]] bool Has ( SemaTypeId Id ) const
            {
                return Id.IsValid() and Id.Value < Types.Size();
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
            /// `Array<T>`, of `Enumerable<T>`, of `map<U>`. A value of type `T`
            /// there has no SemaTypeId, and cannot: `T` only becomes a type when
            /// the definition is instantiated, which is monomorphisation, which is
            /// backend work (rules/core-ast.md). Recorded rather than recomputed,
            /// because TypeChecker is the only thing that knows where those bodies
            /// begin and end; AstInvariant reads it to tell "deferred until
            /// instantiation" apart from "the middle-end forgot".
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

            [[nodiscard]] std::size_t Size () const
            {
                return Types.Size();
            }

            // --- Frontend cache (Issue #61) -----------------------------------
            //
            // Dedup is not serialised: it exists only to make Intern() an O(1)
            // lookup during TypeChecker, and every key is recomputable from an
            // already-loaded Types arena (its entries are never duplicated —
            // Intern() itself ensures that), so DeserializeCache rebuilds it
            // directly instead of persisting a redundant copy.
            void SerializeCache ( Meta::Writer &W ) const;

            // Expects a *fresh* UnitTypes (same contract as DeserializeArena).
            [[nodiscard]] bool DeserializeCache ( Meta::Reader &R );

        private:

            ::Volt::Core::Arena<SemaType, SemaTypeId> Types;
            std::map<std::vector<std::uint32_t>, SemaTypeId> Dedup;
            std::vector<SemaTypeId> OfExpr; // indexed by ExprId::Value
            std::vector<bool> Deferred;     // indexed by ExprId::Value
            std::unordered_map<BindingSite, SemaTypeId, BindingSiteHash> SiteTypes;
        };

    } // namespace TypeSystem

} // namespace MiddleEnd

} // namespace Volt
