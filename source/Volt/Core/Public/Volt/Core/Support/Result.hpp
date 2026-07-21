#pragma once

#include <expected>
#include <utility>

namespace Volt
{

namespace Core
{

    /// Fallible return value. Thin alias over std::expected so passes can
    /// return `Result<T, Diagnostic>` and propagate with `and_then`.
    template <typename T, typename E> using Result = std::expected<T, E>;

    template <typename E> using Failure = std::unexpected<E>;

    template <typename E> [[nodiscard]] constexpr Failure<std::decay_t<E>> Fail ( E &&Error )
    {
        return Failure<std::decay_t<E>>{ std::forward<E>( Error ) };
    }

} // namespace Core

} // namespace Volt
