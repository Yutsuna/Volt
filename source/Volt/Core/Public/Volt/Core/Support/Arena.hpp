#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace Volt
{

    namespace Core
    {

        /// Value-storage arena: elements live contiguously in a vector and are
        /// referenced by a strongly-typed Id (see Id.hpp). No smart pointers,
        /// cache-friendly, and trivially serialisable for hot-reload.
        template <typename T, typename IdType>
        class Arena
        {

        public:

            using ValueType = T;

            [[nodiscard]] IdType Add( T Value )
            {
                const auto Index = static_cast<typename IdType::ValueType>( Storage.size() );
                Storage.push_back( std::move( Value ) );
                return IdType{ Index };
            }

            template <typename... Args>
            [[nodiscard]] IdType Emplace( Args&&... InArgs )
            {
                const auto Index = static_cast<typename IdType::ValueType>( Storage.size() );
                Storage.emplace_back( std::forward<Args>( InArgs )... );
                return IdType{ Index };
            }

            [[nodiscard]] T& Get( IdType Id )
            {
                return Storage[Id.Value];
            }

            [[nodiscard]] const T& Get( IdType Id ) const
            {
                return Storage[Id.Value];
            }

            [[nodiscard]] std::size_t Size() const
            {
                return Storage.size();
            }

            [[nodiscard]] bool IsEmpty() const
            {
                return Storage.empty();
            }

            void Reserve( std::size_t Capacity )
            {
                Storage.reserve( Capacity );
            }

            [[nodiscard]] auto begin()
            {
                return Storage.begin();
            }

            [[nodiscard]] auto end()
            {
                return Storage.end();
            }

            [[nodiscard]] auto begin() const
            {
                return Storage.begin();
            }

            [[nodiscard]] auto end() const
            {
                return Storage.end();
            }

        private:

            std::vector<T> Storage;
        };

    }

}
