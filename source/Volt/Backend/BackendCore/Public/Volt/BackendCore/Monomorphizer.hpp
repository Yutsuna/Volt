#pragma once

// Monomorphizer.hpp — the instantiation queue codegen drains.
//
// A generic body (`Array<T>`, `map<U>`) is typed only after substitution:
// TypeChecker marks its expressions deferred (UnitTypes::MarkDeferred) and
// instantiation is explicitly backend work (rules/core-ast.md). Each
// concrete use enqueues a request here; the emitter drains the queue,
// substituting the request's arguments while walking the generic body, and
// requests discovered during that walk land back in the queue. Keys are
// flattened NominalId trees — the cross-unit currency — so `Array<Int32>`
// instantiates once per build no matter how many units mention it.

#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace Volt
{

namespace Backend
{

    // One requested instantiation: which member, and its concrete bindings.
    // `Base`+`Args` alone (a type-shaped key) is enough for a *layout*
    // (`InstanceLayouts::Of`, keyed independently and without going through
    // this queue at all) but not for a *body*: a generic type can declare
    // several members, and `Array<Int32>` says nothing about which of
    // `push`/`pop`/`map` is meant. `Owner`+`Name` names the member; `Args` is
    // the pre-order flattening of `CalleeEntry::Bindings` — the owner's own
    // generics first, then the method's — the same encoding InstanceLayouts
    // and Mangler read, so a layout, a symbol and a queue entry can never
    // disagree about which instantiation they mean.
    struct MonoRequest
    {

        Sema::NominalId Owner; // invalid for a free function
        Sema::Symbol Name;     // interned in the TypeStore, like Member::Name
        std::vector<std::uint32_t> Args;

        [[nodiscard]] std::vector<std::uint32_t> Key () const
        {
            std::vector<std::uint32_t> Flat;
            Flat.reserve( 2 + Args.size() );
            Flat.push_back( Owner.Value );
            Flat.push_back( Name.Value );
            Flat.insert( Flat.end(), Args.begin(), Args.end() );
            return Flat;
        }
    };

    class Monomorphizer
    {

    public:

        // Enqueue once: a request whose key was already seen is dropped, so
        // recursive generics terminate.
        void Enqueue ( MonoRequest Request )
        {
            if ( Seen.insert( Request.Key() ).second )
            {
                Pending.push_back( std::move( Request ) );
            }
        }

        [[nodiscard]] std::optional<MonoRequest> Next ()
        {
            if ( Pending.empty() )
            {
                return std::nullopt;
            }
            MonoRequest Front = std::move( Pending.front() );
            Pending.pop_front();
            return Front;
        }

        [[nodiscard]] std::size_t InstantiationCount () const
        {
            return Seen.size();
        }

    private:

        std::deque<MonoRequest> Pending;
        std::set<std::vector<std::uint32_t>> Seen;
    };

} // namespace Backend

} // namespace Volt
