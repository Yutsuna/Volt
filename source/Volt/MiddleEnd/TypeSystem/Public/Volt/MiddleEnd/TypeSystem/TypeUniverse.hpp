#pragma once

// TypeUniverse.hpp — the canonical, build-wide flyweight for `SemaType`.
//
// `SemaType` used to be interned once per compile unit, which made a
// `SemaTypeId` mean something only inside the arena that minted it. Two units
// each writing `G<A>` got two unrelated handles, so the only cross-unit
// currency left was `NominalId` — and a `NominalId` deliberately drops the
// arguments. Every question of the form "are these two instantiations the
// same one" therefore had to be answered by re-walking the argument lists, or
// (worse) by comparing base nominals and silently accepting `G<A>` where
// `G<B>` was meant.
//
// The universe removes the question instead of answering it repeatedly: one
// dictionary per build, structural dedup, so `SemaTypeId` equality *is* type
// equality everywhere — inside a unit, across units, and inside a
// monomorphisation overlay. `UnitTypes` keeps only the per-unit *mappings*
// (which expression has which type); the types themselves live here.
//
// ## Scope: one universe per `TypeStore`, never one per process
//
// A `SemaType` is `(NominalId, args...)`, and a `NominalId` is an index into
// one `TypeStore`. Two stores coexist in a single process (`DriverBuild.cpp`
// compiles an isolated stdlib while the outer build is live), and their
// nominal spaces are unrelated — so a process-global dictionary would hand
// the isolated build a handle whose base names a different type in its own
// store. The universe is therefore owned by the `TypeStore` whose ids it
// speaks (`TypeStore::Universe()`), which is exactly the scope in which
// "canonical" is a meaningful word.
//
// ## Concurrency
//
// `TypeChecker` runs in parallel, one thread per unit, and every one of them
// interns. Writes take a mutex; reads take nothing at all:
//
//   * storage is a table of fixed-size chunks, so an append never moves an
//     element that was already published — a `const SemaType &` handed out
//     stays valid for the life of the universe;
//   * `Published` is the release/acquire fence between "the slot is written"
//     and "the id is visible", so a reader that accepts an id has, by that
//     fact, already seen the bytes behind it.
//
// `UnitTypes::Intern` additionally memoises per unit, so the mutex is touched
// only the first time a given unit mentions a given shape.
//
// ## Determinism
//
// Ids are handed out densely, in intern order, and an argument is always
// interned before the type that carries it — so a serialised universe replays
// into an identical numbering (`DeserializeCache` asserts exactly that). That
// is what lets the frontend cache persist a unit's `ExprId -> SemaTypeId`
// mapping without a remap table, the same way `NominalId`/`SigTypeId` already
// survive a cache round-trip.

#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndTypeSystem_export.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
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

        // The canonical dictionary described at the top of this file. Owned by
        // a `TypeStore`; reached through `TypeStore::Universe()`.
        class VOLT_MIDDLEEND_TYPESYSTEM_EXPORT TypeUniverse
        {

        public:

            TypeUniverse ()                                 = default;
            TypeUniverse ( const TypeUniverse & )           = delete;
            TypeUniverse ( TypeUniverse && )                = delete;
            TypeUniverse &operator=( const TypeUniverse & ) = delete;
            TypeUniverse &operator=( TypeUniverse && )      = delete;
            ~TypeUniverse ();

            // The structural key of a type — its base and its arguments, flat.
            // Public because `UnitTypes` memoises on the same encoding, and two
            // spellings of one key is one bug waiting to happen.
            [[nodiscard]] static std::vector<std::uint32_t> KeyOf ( const SemaType &Value )
            {
                std::vector<std::uint32_t> Key;
                Key.reserve( 1 + Value.Args.Size() );
                Key.push_back( Value.Base.Value );
                for ( const SemaTypeId Arg : Value.Args )
                {
                    Key.push_back( Arg.Value );
                }
                return Key;
            }

            // The canonical handle for `Value`, minting one if this build has
            // never seen the shape. Invalid in, invalid out — an un-inferred
            // type is a normal outcome, not something to register.
            [[nodiscard]] SemaTypeId Intern ( SemaType Value );

            // Never dangles: chunk storage is append-only and never moves a
            // published element. An unknown id reads as the empty type rather
            // than out of bounds — callers that care ask `Has` first, and the
            // ones that do not were relying on `Base.IsValid()` anyway.
            [[nodiscard]] const SemaType &Get ( SemaTypeId Id ) const
            {
                if ( not Has( Id ) )
                {
                    return Empty();
                }
                const SemaType *Chunk = Chunks[Id.Value >> ChunkBits].load( std::memory_order_acquire );
                return Chunk[Id.Value & ChunkMask];
            }

            [[nodiscard]] bool Has ( SemaTypeId Id ) const
            {
                return Id.IsValid() and Id.Value < Published.load( std::memory_order_acquire );
            }

            [[nodiscard]] std::size_t Size () const
            {
                return Published.load( std::memory_order_acquire );
            }

            // The full generic signature, arguments included — `G<A>`, not `G`.
            // Diagnostics render a type through this and nothing else: printing
            // the base nominal alone is what made a mismatch between two
            // instantiations of one generic read as "cannot assign G to G".
            [[nodiscard]] std::string Describe ( const TypeStore &Store, SemaTypeId Id ) const;

            // Back to the just-constructed state. The frontend cache's recovery
            // path needs it for the same reason `TypeStore::Clear` exists: a
            // failed replay may leave a partially populated dictionary.
            void Clear ();

            void SerializeCache ( Meta::Writer &W ) const;

            // Expects a *fresh* universe (same contract as DeserializeArena) and
            // replays in intern order, so every id comes back identical.
            [[nodiscard]] bool DeserializeCache ( Meta::Reader &R );

            // The type an unknown handle reads as. Shared rather than
            // per-instance so `UnitTypes` can hand it back before it is bound.
            [[nodiscard]] static const SemaType &Empty ();

        private:

            // 1024 entries per chunk, 16M canonical types in all. A chunk is
            // allocated on first use, so a small build pays for one.
            static constexpr std::uint32_t ChunkBits = 10;
            static constexpr std::uint32_t ChunkSize = 1U << ChunkBits;
            static constexpr std::uint32_t ChunkMask = ChunkSize - 1U;
            static constexpr std::size_t MaxChunks   = 16384;

            mutable std::mutex Writes;
            std::unordered_map<std::vector<std::uint32_t>, SemaTypeId, U32KeyHash> Dedup;
            // Ownership lives here (guarded by `Writes`); `Chunks` only
            // *publishes* the same pointers, so a reader never touches a vector
            // that a concurrent append may be reallocating.
            std::vector<std::unique_ptr<SemaType[]>> Owned;
            std::array<std::atomic<SemaType *>, MaxChunks> Chunks{};
            std::atomic<std::uint32_t> Published{ 0 };
        };

    } // namespace TypeSystem

} // namespace MiddleEnd

} // namespace Volt
