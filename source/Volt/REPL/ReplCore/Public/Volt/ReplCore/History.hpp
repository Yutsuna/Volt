#pragma once

// History.hpp — the statements a session has evaluated, in order.
//
// Small enough to be obvious and separate enough to be tested: a list, a
// bounded one, with the one non-trivial question a REPL asks of it — reverse
// search, for `^R`. Reading a history file and writing one is I/O and
// therefore ReplTui's; what a file is *read into* is this.

#include "ReplCore_export.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Repl
{

    class REPLCORE_EXPORT History
    {

    public:

        // Beyond this many entries the oldest go. A REPL that kept everything
        // would eventually read a megabyte at startup to offer a line from
        // last March.
        static constexpr std::size_t DefaultLimit = 2000;

        explicit History ( std::size_t InLimit = DefaultLimit ) : Limit( InLimit == 0 ? DefaultLimit : InLimit )
        {
        }

        // Remember a statement. Blank lines and an immediate repeat of the
        // last entry are dropped: neither is something anyone wants to arrow
        // back through.
        void Add ( std::string Statement );

        [[nodiscard]] std::span<const std::string> All () const
        {
            return Entries;
        }

        [[nodiscard]] std::size_t Size () const
        {
            return Entries.size();
        }

        [[nodiscard]] std::string_view At ( std::size_t Index ) const;

        // The newest entry at or before `From` that contains `Needle`, for
        // `^R`. Nothing when there is no match — including when `Needle` is
        // empty, since matching everything is not a search.
        [[nodiscard]] std::optional<std::size_t> SearchBackwards ( std::string_view Needle, std::size_t From ) const;

    private:

        std::vector<std::string> Entries;
        std::size_t Limit;
    };

} // namespace Repl

} // namespace Volt
