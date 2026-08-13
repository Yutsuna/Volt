#pragma once

// InstantiationCache.hpp — memoised monomorphisation.
//
// `ReinstantiateBody` (TypeSystem/Instantiate.hpp) re-runs the whole expression
// inferencer over one generic body under one set of concrete bindings. That is
// the single most expensive thing the middle-end does on behalf of a backend,
// and it is a *pure function of its key*: the same body, under the same
// bindings, in the same build, always produces the same overlay. So it is
// computed once and remembered.
//
// ## What the key has to contain, and why
//
// `(template, arguments)` is the shape the design called for, and it is not
// quite enough on its own:
//
//   * a `DeclId` indexes the arena of the unit that minted it, so two units'
//     declarations collide on the bare id — hence `Unit`, straight off
//     `Member::Unit`;
//   * an inherited method is *one* declaration reached through several
//     receivers (a mixin's body, included by two different types), and each
//     receiver binds a different `self` — hence `Owner`;
//   * a `SemaTypeId` is canonical within one `TypeStore` and meaningless
//     across two, and two stores do coexist in a process (`DriverBuild.cpp`
//     compiles an isolated stdlib while the outer build is live) — hence
//     `Generation`, which makes a key from another build a miss instead of a
//     wrong answer.
//
// The arguments themselves are `TypeUniverse` handles, which is what makes the
// key cheap: two instantiations agree iff their handles do, with no argument
// tree to walk.
//
// ## Budget
//
// An overlay is not small — it carries a type per expression of the declaring
// unit — so the cache is bounded and evicts least-recently-used. A memo that
// grows without limit trades a bounded amount of work for an unbounded amount
// of memory, which is not a trade a compiler should make silently.
//
// ## Cycles
//
// `Enter`/`Leave` mark a key in flight. A body that re-entered its own
// instantiation would otherwise recurse forever; the guard turns that into a
// bounded, reported outcome (an empty overlay) instead. Nothing drives
// `ReinstantiateBody` recursively today — the backend queues further requests
// rather than nesting them — so this is a standing guarantee, not a live
// workaround.

#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/MiddleEnd/TypeSystem/Instantiate.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndAnalysis_export.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Volt
{

namespace MiddleEnd
{

    namespace Analysis
    {

        // One monomorphisation request, canonically. See the header comment for
        // why each field is load-bearing.
        struct MonoKey
        {

            std::uint64_t Generation = 0; // TypeStore::Generation — which build minted the ids below
            std::uint32_t Unit       = 0; // Member::Unit — the arena `Template` indexes
            Frontend::DeclId Template;    // the generic body
            TypeSystem::NominalId Owner;  // the receiver's nominal, which binds `self`
            ::Volt::Core::SmallVec<TypeSystem::SemaTypeId, 4> CanonicalArgs;

            [[nodiscard]] bool operator==( const MonoKey &Other ) const
            {
                if ( Generation != Other.Generation or Unit != Other.Unit or Template != Other.Template or Owner != Other.Owner )
                {
                    return false;
                }
                if ( CanonicalArgs.Size() != Other.CanonicalArgs.Size() )
                {
                    return false;
                }
                for ( std::size_t Index = 0; Index < CanonicalArgs.Size(); ++Index )
                {
                    if ( CanonicalArgs[Index] != Other.CanonicalArgs[Index] )
                    {
                        return false;
                    }
                }
                return true;
            }
        };

        struct MonoKeyHash
        {

            [[nodiscard]] std::size_t operator()( const MonoKey &Key ) const noexcept
            {
                std::size_t Hash = 1469598103934665603ULL;
                const auto Mix   = [&Hash] ( const std::uint64_t Word )
                {
                    Hash ^= Word;
                    Hash *= 1099511628211ULL;
                };

                Mix( Key.Generation );
                Mix( Key.Unit );
                Mix( Key.Template.Value );
                Mix( Key.Owner.Value );
                for ( const TypeSystem::SemaTypeId Arg : Key.CanonicalArgs )
                {
                    Mix( Arg.Value );
                }
                return Hash;
            }
        };

        // Process-wide, because it outlives any one `ReinstantiateBody` call and
        // has no natural owner below the build — `Generation` in the key is what
        // keeps two coexisting builds from reading each other's entries.
        class VOLT_MIDDLEEND_ANALYSIS_EXPORT InstantiationCache
        {

        public:

            [[nodiscard]] static InstantiationCache &Global ();

            // The overlay this key already produced, if any. Copied out under
            // the lock: an eviction must never pull storage out from under a
            // caller still reading it.
            [[nodiscard]] std::optional<TypeSystem::InstantiatedBody> Lookup ( const MonoKey &Key );

            void Insert ( MonoKey Key, const TypeSystem::InstantiatedBody &Body );

            // False when `Key` is already being instantiated on this path — a
            // recursive instantiation cycle. The caller returns what it has
            // rather than recursing.
            [[nodiscard]] bool Enter ( const MonoKey &Key );
            void Leave ( const MonoKey &Key );

            // Largest number of overlays kept. Shrinking evicts immediately.
            void SetBudget ( std::size_t MaxEntries );

            void Clear ();

            [[nodiscard]] std::size_t Size () const;

            [[nodiscard]] std::size_t Hits () const
            {
                return HitCount.load( std::memory_order_relaxed );
            }

            [[nodiscard]] std::size_t Misses () const
            {
                return MissCount.load( std::memory_order_relaxed );
            }

            [[nodiscard]] std::size_t Cycles () const
            {
                return CycleCount.load( std::memory_order_relaxed );
            }

            [[nodiscard]] std::size_t Evictions () const
            {
                return EvictCount.load( std::memory_order_relaxed );
            }

        private:

            struct Entry
            {

                TypeSystem::InstantiatedBody Body;
                std::uint64_t Tick = 0; // last use, for LRU
            };

            // Caller holds `Lock`.
            void EvictDownTo ( std::size_t MaxEntries );

            mutable std::mutex Lock;
            std::unordered_map<MonoKey, Entry, MonoKeyHash> Entries;
            std::unordered_set<MonoKey, MonoKeyHash> InFlight;
            std::uint64_t Clock = 0;
            std::size_t Budget  = 256;
            std::atomic<std::size_t> HitCount{ 0 };
            std::atomic<std::size_t> MissCount{ 0 };
            std::atomic<std::size_t> CycleCount{ 0 };
            std::atomic<std::size_t> EvictCount{ 0 };
        };

    } // namespace Analysis

} // namespace MiddleEnd

} // namespace Volt
