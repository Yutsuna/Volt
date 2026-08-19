#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Volt::Frontend
{

struct UnitEntry
{
    std::string_view Suffix;
    std::uint64_t IntMultiplier;
    double FloatMultiplier;
    bool bIsFloat;
};

#define VOLT_UNIT( Suffix, Family, Multiplier, IsFloat )                                                                         \
    UnitEntry{ Suffix, static_cast<std::uint64_t>( Multiplier ), static_cast<double>( Multiplier ), IsFloat },
inline constexpr auto UnitTable = std::to_array<UnitEntry>( {
#include "Volt/Frontend/Lexer/Units.inl"
} );

struct UnitFoldResult
{
    std::size_t WrittenSize = 0;
    bool bFloat             = false;
    bool bOverflow          = false;
};

[[nodiscard]] const UnitEntry *MatchUnitSuffix ( std::string_view Source, std::size_t Pos );

[[nodiscard]] UnitFoldResult
FoldUnitLiteral ( std::string_view CleanDigits, bool bHasFraction, const UnitEntry &Unit, char *OutBuf, std::size_t OutCap );

} // namespace Volt::Frontend
