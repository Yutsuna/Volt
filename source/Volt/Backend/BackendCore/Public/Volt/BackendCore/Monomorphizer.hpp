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

    // One requested instantiation: the generic declaration plus its concrete
    // arguments, each itself a flattened pre-order NominalId spelling so
    // nested generics (`Array<Array<Int32>>`) key exactly.
    struct MonoRequest
    {

        Sema::NominalId Base;
        // Pre-order flattening of the argument type trees: for each node,
        // its NominalId then its own argument count.
        std::vector<std::uint32_t> Args;

        [[nodiscard]] std::vector<std::uint32_t> Key () const
        {
            std::vector<std::uint32_t> Flat;
            Flat.reserve( 1 + Args.size() );
            Flat.push_back( Base.Value );
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
