#pragma once

namespace Volt
{

namespace Meta
{

    /**
     * @struct Overloaded
     * @brief The classic overload set for std::visit
     * @details combine per-alternative lambdas into one callable.
     *          a pass is then just an Overloaded{...}
     *          with a generic `[](auto&)` fallback that walks fields via Reflect.
     */
    template <typename... Fs> struct Overloaded : Fs...
    {

        using Fs::operator()...;
    };

    /** @brief Deduction guide for Overloaded */
    template <typename... Fs> Overloaded( Fs... ) -> Overloaded<Fs...>;

} // namespace Meta

} // namespace Volt
