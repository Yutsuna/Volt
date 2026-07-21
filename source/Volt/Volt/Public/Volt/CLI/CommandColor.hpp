#pragma once

#include <string>
#include <string_view>

namespace Volt::CLI
{

inline constexpr std::string_view AnsiReset      = "\x1b[0m";
inline constexpr std::string_view AnsiYellowBold = "\x1b[1;33m";
inline constexpr std::string_view AnsiGreen      = "\x1b[32m";

[[nodiscard]] inline std::string Paint ( std::string_view Code, std::string_view Text, bool bColor )
{
    if ( !bColor )
    {
        return std::string( Text );
    }
    return std::string( Code ) + std::string( Text ) + std::string( AnsiReset );
}

} // namespace Volt::CLI
