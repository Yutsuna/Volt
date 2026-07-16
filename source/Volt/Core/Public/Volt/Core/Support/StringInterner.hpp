#pragma once

#include "Volt/Core/Support/Id.hpp"

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Volt
{

    namespace Core
    {

        struct SymbolTag
        {
        };

        /// Interned string handle. Equal strings share one Symbol, so identity
        /// comparison is a single integer compare.
        using Symbol = TypedId<SymbolTag>;

        /// Deduplicating string table. Owns the character storage; hands out
        /// Symbols that stay valid for the interner's lifetime.
        class StringInterner
        {

        public:

            StringInterner()
            {
                // Reserve slot 0 for the empty string so a default Symbol
                // that happens to be valid still resolves sensibly.
                static_cast<void>( Intern( std::string_view{} ) );
            }

            [[nodiscard]] Symbol Intern( std::string_view Text )
            {
                if ( const auto It = Lookup.find( Text ); It != Lookup.end() )
                {
                    return It->second;
                }

                const auto Index = static_cast<Symbol::ValueType>( Storage.size() );
                Storage.emplace_back( Text );

                const Symbol Handle{ Index };
                Lookup.emplace( std::string_view{ Storage.back() }, Handle );
                return Handle;
            }

            [[nodiscard]] std::string_view Resolve( Symbol Handle ) const
            {
                return Storage[Handle.Value];
            }

            [[nodiscard]] std::size_t Size() const
            {
                return Storage.size();
            }

        private:

            // std::deque keeps element addresses stable across growth, so the
            // string_view keys in Lookup (and Resolve results) never dangle.
            std::deque<std::string> Storage;
            std::unordered_map<std::string_view, Symbol> Lookup;
        };

    }

}
