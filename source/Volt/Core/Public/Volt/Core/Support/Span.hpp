#pragma once

#include <cstddef>
#include <span>

namespace Volt
{

    namespace Core
    {

        /// Non-owning contiguous view. Alias kept in Support so the rest of the
        /// codebase depends on `Volt::Core::Span` rather than <span> directly.
        template <typename T, std::size_t Extent = std::dynamic_extent>
        using Span = std::span<T, Extent>;

    }

}
