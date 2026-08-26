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

#include "BackendCore_export.hpp"
#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <cstdint>
#include <deque>
#include <functional>
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

        MiddleEnd::TypeSystem::NominalId Owner; // invalid for a free function
        Volt::Core::Symbol Name;                // interned in the TypeStore, like Member::Name
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

    // The two halves of draining that are the same for every target: the loop,
    // and which member a request names. Both are pure scheduling and pure store
    // reads — no emitted code, no target type — so they live here rather than
    // once per backend. `Emit` is where a target's own instantiation goes.
    namespace MonoQueue
    {

        // To a fixpoint, not once: a drained body can itself enqueue further
        // requests (a generic method calling another generic method), and
        // Enqueue dedupes on MonoRequest::Key, so the loop terminates exactly
        // when no new instantiation was discovered. `ShouldStop` is polled
        // between requests so a failed emission unwinds without the caller
        // re-checking inside `Emit`.
        //
        // One indirect call per instantiation, never per node
        // (core-interfaces.md).
        BACKENDCORE_EXPORT void Drain ( Monomorphizer &Queue,
                                        const std::function<bool()> &ShouldStop,
                                        const std::function<void( const MonoRequest & )> &Emit );

        // The member a request names, plus the UnitView that declares it (its
        // Ast/Scopes are what a re-instantiated body is typed against). Null
        // when the store declares no such member — a middle-end contract
        // violation, reported by the caller.
        //
        // A MonoRequest names the *receiver*, not the declaring type, so the
        // lookup goes through LookupMember's own order — own body first, then
        // mixins, then the superclass. That is exactly what makes an inherited
        // default (`Arithmetic#min` called on `Int32`) drainable: the request's
        // Owner never declares the member itself.
        [[nodiscard]] BACKENDCORE_EXPORT const MiddleEnd::TypeSystem::Member *
        Lookup ( const BackendInput &Build, const MonoRequest &Request, const UnitView **OutUnit );

    } // namespace MonoQueue

} // namespace Backend

} // namespace Volt
