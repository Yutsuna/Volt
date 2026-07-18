#pragma once

#include <string_view>

namespace Volt
{

namespace CLI
{

    /// The toolchain version stamped into generated manifests (`runtime "..."`).
    inline constexpr std::string_view VoltVersion = "0.1.0";

} // namespace CLI

} // namespace Volt
