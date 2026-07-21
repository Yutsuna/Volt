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
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace Volt
{

namespace Sema
{

    struct SemaTypeTag
    {
    };

    using SemaTypeId = Core::TypedId<SemaTypeTag>;

    // An expression's type: a nominal plus its (already concrete) arguments.
    // An invalid Base means "not inferred", which is a normal outcome and
    // never on its own a diagnostic.
    struct SemaType
    {

        NominalId Base;
        Core::SmallVec<SemaTypeId, 2> Args;
    };

    // Every type one compile unit inferred. Mutated only by that unit's own
    // pass run, so the parallel sema phase needs no lock: unlike the store,
    // this is per-file state living on the CompileUnit.
    class UnitTypes
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

        [[nodiscard]] std::size_t Size () const
        {
            return Types.Size();
        }

    private:

        Core::Arena<SemaType, SemaTypeId> Types;
        std::map<std::vector<std::uint32_t>, SemaTypeId> Dedup;
        std::vector<SemaTypeId> OfExpr; // indexed by ExprId::Value
    };

} // namespace Sema

} // namespace Volt
