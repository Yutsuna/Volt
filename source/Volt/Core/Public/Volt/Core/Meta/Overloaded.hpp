#pragma once

namespace Volt
{

namespace Meta
{

    /// The classic overload set for std::visit: combine per-alternative
    /// lambdas into one callable. A pass is then just an Overloaded{...}
    /// with a generic `[](auto&)` fallback that walks fields via Reflect.
    template <typename... Fs> struct Overloaded : Fs...
    {

        using Fs::operator()...;
    };

    template <typename... Fs> Overloaded( Fs... ) -> Overloaded<Fs...>;

} // namespace Meta

} // namespace Volt
