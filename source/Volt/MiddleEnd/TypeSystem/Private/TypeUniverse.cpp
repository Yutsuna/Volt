// TypeUniverse.cpp — the build-wide canonical dictionary for SemaType, plus
// the `TypeStore` plumbing that owns one.
//
// The write path is the only place a mutex is taken. The read path
// (`TypeUniverse::Get`/`Has`, header-inline) takes nothing: chunk storage never
// moves a published element, and `Published` is the release/acquire fence that
// makes "this id exists" imply "the bytes behind it are visible". See
// TypeUniverse.hpp for the full argument.

#include "Volt/MiddleEnd/TypeSystem/TypeUniverse.hpp"

#include "Volt/Core/Meta/Serialize.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace
{

using Volt::MiddleEnd::TypeSystem::SemaType;
using Volt::MiddleEnd::TypeSystem::SemaTypeId;
using Volt::MiddleEnd::TypeSystem::TypeStore;
using Volt::MiddleEnd::TypeSystem::TypeUniverse;

// Distinguishes one TypeStore instance from every other the process builds.
// Starts at 1 so a default-constructed key (generation 0) never names a real
// build — see Analysis::InstantiationCache, its only consumer.
std::atomic<std::uint64_t> NextGeneration{ 1 };

// A nested slot the declaration side left open reads as `_`, not as the
// top-level "<unresolved>": that is what `IsAssignable` already treats it as —
// a hole that matches whatever fills it — and `G<_>` says so where
// `G<<unresolved>>` only confuses.
[[nodiscard]] std::string
DescribeAt ( const TypeUniverse &Universe, const TypeStore &Store, SemaTypeId Id, const std::uint32_t Depth )
{
    if ( not Universe.Has( Id ) or Depth > 16 )
    {
        return "Void";
    }

    const SemaType &Value = Universe.Get( Id );
    if ( not Value.Base.IsValid() or Value.Base.Value >= Store.TypeCount() )
    {
        return "Void";
    }

    std::string Out{ Store.Text( Store.Type( Value.Base ).Name ) };
    if ( Value.Args.IsEmpty() )
    {
        return Out;
    }

    Out += '<';
    for ( std::size_t Index = 0; Index < Value.Args.Size(); ++Index )
    {
        if ( Index > 0 )
        {
            Out += ", ";
        }
        Out += DescribeAt( Universe, Store, Value.Args[Index], Depth + 1 );
    }
    Out += '>';
    return Out;
}

} // namespace

namespace Volt
{

namespace MiddleEnd
{

    namespace TypeSystem
    {

        TypeUniverse::~TypeUniverse ()
        {
            Clear();
        }

        const SemaType &TypeUniverse::Empty ()
        {
            static const SemaType Value{};
            return Value;
        }

        SemaTypeId TypeUniverse::Intern ( SemaType Value )
        {
            if ( not Value.Base.IsValid() )
            {
                return SemaTypeId{};
            }

            std::vector<std::uint32_t> Key = KeyOf( Value );

            const std::lock_guard<std::mutex> Guard( Writes );
            if ( const auto It = Dedup.find( Key ); It != Dedup.end() )
            {
                return It->second;
            }

            const std::uint32_t Next     = Published.load( std::memory_order_relaxed );
            const std::size_t ChunkIndex = Next >> ChunkBits;
            if ( ChunkIndex >= MaxChunks )
            {
                return SemaTypeId{}; // 16M canonical types in one build: not a shape any program reaches.
            }

            SemaType *Chunk = Chunks[ChunkIndex].load( std::memory_order_relaxed );
            if ( Chunk == nullptr )
            {
                Owned.push_back( std::make_unique<SemaType[]>( ChunkSize ) );
                Chunk = Owned.back().get();
                Chunks[ChunkIndex].store( Chunk, std::memory_order_release );
            }
            Chunk[Next & ChunkMask] = std::move( Value );

            const SemaTypeId Id{ Next };
            Dedup.emplace( std::move( Key ), Id );

            // Release *after* the slot is written: a reader that accepts this id
            // has, by that acquire, already seen both the chunk pointer and the
            // element behind it.
            Published.store( Next + 1, std::memory_order_release );
            return Id;
        }

        std::string TypeUniverse::Describe ( const TypeStore &Store, SemaTypeId Id ) const
        {
            return DescribeAt( *this, Store, Id, 0 );
        }

        void TypeUniverse::Clear ()
        {
            const std::lock_guard<std::mutex> Guard( Writes );
            Published.store( 0, std::memory_order_release );
            for ( std::atomic<SemaType *> &Slot : Chunks )
            {
                Slot.store( nullptr, std::memory_order_release );
            }
            Owned.clear();
            Dedup.clear();
        }

        void TypeUniverse::SerializeCache ( Meta::Writer &W ) const
        {
            const auto Count = static_cast<std::uint32_t>( Size() );
            Meta::Serialize( W, Count );
            for ( std::uint32_t Index = 0; Index < Count; ++Index )
            {
                const SemaType &Value = Get( SemaTypeId{ Index } );
                Meta::Serialize( W, Value.Base );
                Meta::Serialize( W, Value.Args );
            }
        }

        bool TypeUniverse::DeserializeCache ( Meta::Reader &R )
        {
            Clear();

            std::uint32_t Count = 0;
            if ( not Meta::Deserialize( R, Count ) )
            {
                return false;
            }

            for ( std::uint32_t Index = 0; Index < Count; ++Index )
            {
                SemaType Value;
                if ( not Meta::Deserialize( R, Value.Base ) or not Meta::Deserialize( R, Value.Args ) )
                {
                    return false;
                }

                // Replay must reproduce the original numbering exactly — that is
                // the whole reason a unit's ExprId -> SemaTypeId mapping needs no
                // remap table. An id that comes back different means the stream
                // was written by an incompatible encoding (or is corrupt), and the
                // caller's recovery path is an ordinary cache miss.
                if ( Intern( std::move( Value ) ).Value != Index )
                {
                    return false;
                }
            }
            return true;
        }

        // --- TypeStore's half of the ownership --------------------------------

        TypeStore::TypeStore ()
            : UniverseStorage( std::make_unique<TypeUniverse>() ), Gen( NextGeneration.fetch_add( 1, std::memory_order_relaxed ) )
        {
        }

        TypeStore::~TypeStore () = default;

        void TypeStore::ClearUniverse ()
        {
            UniverseStorage->Clear();
        }

    } // namespace TypeSystem

} // namespace MiddleEnd

} // namespace Volt
