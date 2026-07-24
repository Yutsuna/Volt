#pragma once

#include "Volt/Core/Support/Id.hpp"

#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

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

        StringInterner ()
        {
            // Reserve slot 0 for the empty string so a default Symbol
            // that happens to be valid still resolves sensibly.
            const Symbol _ = Intern( std::string_view{} );
        }

        [[nodiscard]] Symbol Intern ( std::string_view Text )
        {
            if ( const auto It = Lookup.find( Text ); It != Lookup.end() )
            {
                return It->second;
            }

            const auto Index          = static_cast<Symbol::ValueType>( Views.size() );
            const std::string_view In = Store( Text );
            Views.push_back( In );

            const Symbol Handle{ Index };
            Lookup.emplace( In, Handle );
            return Handle;
        }

        /// The Symbol for Text if it was already interned, without adding it.
        /// Lets read-only lookups miss without growing the table.
        [[nodiscard]] std::optional<Symbol> Find ( std::string_view Text ) const
        {
            if ( const auto It = Lookup.find( Text ); It != Lookup.end() )
            {
                return It->second;
            }
            return std::nullopt;
        }

        /// An invalid handle resolves to the empty view rather than reading out
        /// of bounds: a node whose name failed to parse still reaches every
        /// consumer downstream (the driver runs Sema over a unit that reported
        /// syntax errors), and a bare `Views[Handle.Value]` turns that into UB.
        [[nodiscard]] std::string_view Resolve ( Symbol Handle ) const
        {
            return Handle.IsValid() and Handle.Value < Views.size() ? Views[Handle.Value] : std::string_view{};
        }

        [[nodiscard]] std::size_t Size () const
        {
            return Views.size();
        }

    private:

        [[nodiscard]] std::string_view Store ( std::string_view Text )
        {
            if ( Text.empty() )
            {
                return {};
            }
            char *Bytes = static_cast<char *>( Buffer.allocate( Text.size(), alignof( char ) ) );
            std::memcpy( Bytes, Text.data(), Text.size() );
            return { Bytes, Text.size() };
        }

        // Bump allocator: characters are packed into geometrically growing
        // blocks that are never released before the interner dies, so every
        // string_view handed out stays valid — without one heap node per
        // string as the previous std::deque<std::string> storage had.
        std::pmr::monotonic_buffer_resource Buffer{ 64UL * 1024UL };
        std::vector<std::string_view> Views;
        std::unordered_map<std::string_view, Symbol> Lookup;
    };

} // namespace Core

} // namespace Volt
