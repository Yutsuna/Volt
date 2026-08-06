#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#if defined( VOLT_CHECKED_IDS )
    #include <atomic>
    #include <cstdint>
    #include <cstdlib>
    #include <print>
#endif

namespace Volt
{

namespace Core
{

    /// Value-storage arena: elements live contiguously in a vector and are
    /// referenced by a strongly-typed Id (see Id.hpp). No smart pointers,
    /// cache-friendly, and trivially serialisable for hot-reload.
    ///
    /// Under VOLT_CHECKED_IDS every arena instance owns a process-unique tag,
    /// stamped on the Ids it mints; Get aborts on an Id minted by another
    /// arena instead of silently reading a wrong node. Zero overhead and
    /// identical Id layout in normal builds.
    template <typename T, typename IdType> class Arena
    {

    public:

        using ValueType = T;

        [[nodiscard]] IdType Add ( T Value )
        {
            const auto Index = static_cast<typename IdType::ValueType>( Storage.size() );
            Storage.push_back( std::move( Value ) );
            return MakeId( Index );
        }

        template <typename... Args> [[nodiscard]] IdType Emplace ( Args &&...InArgs )
        {
            const auto Index = static_cast<typename IdType::ValueType>( Storage.size() );
            Storage.emplace_back( std::forward<Args>( InArgs )... );
            return MakeId( Index );
        }

        /// Stamped Id for an existing slot (index-based iteration).
        [[nodiscard]] IdType MakeId ( typename IdType::ValueType Index ) const
        {
            IdType Id{ Index };
#if defined( VOLT_CHECKED_IDS )
            Id.Provenance = Tag;
#endif
            return Id;
        }

        [[nodiscard]] T &Get ( IdType Id )
        {
            CheckId( Id );
            return Storage[Id.Value];
        }

        [[nodiscard]] const T &Get ( IdType Id ) const
        {
            CheckId( Id );
            return Storage[Id.Value];
        }

        [[nodiscard]] std::size_t Size () const
        {
            return Storage.size();
        }

        [[nodiscard]] bool IsEmpty () const
        {
            return Storage.empty();
        }

        void Reserve ( std::size_t Capacity )
        {
            Storage.reserve( Capacity );
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        [[nodiscard]] auto begin ()
        {
            return Storage.begin();
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        [[nodiscard]] auto end ()
        {
            return Storage.end();
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        [[nodiscard]] auto begin () const
        {
            return Storage.begin();
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        [[nodiscard]] auto end () const
        {
            return Storage.end();
        }

    private:

        void CheckId ( [[maybe_unused]] IdType Id ) const
        {
#if defined( VOLT_CHECKED_IDS )
            const bool bForeign = Id.Provenance != 0 and Id.Provenance != Tag;
            if ( bForeign or Id.Value >= Storage.size() )
            {
                std::println( stderr, "Volt: TypedId %u (arena tag %u) used with arena tag %u of size %zu\n", Id.Value,
                              Id.Provenance, Tag, Storage.size() );
                std::abort();
            }
#endif
        }

#if defined( VOLT_CHECKED_IDS )
        [[nodiscard]] static std::uint32_t NextArenaTag ()
        {
            static std::atomic<std::uint32_t> Counter{ 0 };
            return Counter.fetch_add( 1, std::memory_order_relaxed ) + 1;
        }

        std::uint32_t Tag = NextArenaTag();
#endif

        std::vector<T> Storage;
    };

} // namespace Core

} // namespace Volt
