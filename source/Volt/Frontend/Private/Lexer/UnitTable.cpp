#include "Volt/Frontend/Lexer/UnitTable.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace Volt::Frontend
{

namespace
{

    [[nodiscard]] inline bool IsIdentCont ( char C )
    {
        return ( C >= 'a' and C <= 'z' ) or ( C >= 'A' and C <= 'Z' ) or C == '_' or ( C >= '0' and C <= '9' );
    }

} // namespace

const UnitEntry *MatchUnitSuffix ( std::string_view Source, std::size_t Pos )
{
    for ( const auto &Unit : UnitTable )
    {
        if ( Pos + Unit.Suffix.size() <= Source.size() and Source.substr( Pos, Unit.Suffix.size() ) == Unit.Suffix )
        {
            const bool bAlpha =
                ( Unit.Suffix[0] >= 'a' and Unit.Suffix[0] <= 'z' ) or ( Unit.Suffix[0] >= 'A' and Unit.Suffix[0] <= 'Z' );
            if ( bAlpha and Pos + Unit.Suffix.size() < Source.size() )
            {
                const char NextChar = Source[Pos + Unit.Suffix.size()];
                if ( IsIdentCont( NextChar ) )
                {
                    const bool bTypedSuffix =
                        NextChar == '_' and Pos + Unit.Suffix.size() + 1 < Source.size() and
                        ( Source[Pos + Unit.Suffix.size() + 1] == 'u' or Source[Pos + Unit.Suffix.size() + 1] == 'i' or
                          Source[Pos + Unit.Suffix.size() + 1] == 'f' );
                    if ( not bTypedSuffix )
                    {
                        continue;
                    }
                }
            }
            return &Unit;
        }
    }
    return nullptr;
}

UnitFoldResult
FoldUnitLiteral ( std::string_view CleanDigits, bool bHasFraction, const UnitEntry &Unit, char *OutBuf, std::size_t OutCap )
{
    UnitFoldResult Res;

    if ( not bHasFraction and not Unit.bIsFloat )
    {
        std::uint64_t IntVal    = 0;
        const auto [Ptr, Error] = std::from_chars( CleanDigits.data(), CleanDigits.data() + CleanDigits.size(), IntVal );
        if ( Error == std::errc{} )
        {
            std::uint64_t Product = 0;
            if ( __builtin_mul_overflow( IntVal, Unit.IntMultiplier, &Product ) )
            {
                Res.bOverflow = true;
                Product       = IntVal;
            }
            const auto [EndPtr, ToErr] = std::to_chars( OutBuf, OutBuf + OutCap, Product );
            Res.WrittenSize            = static_cast<std::size_t>( EndPtr - OutBuf );
            Res.bFloat                 = false;
            return Res;
        }
    }

    double FloatVal         = 0.0;
    const auto [Ptr, Error] = std::from_chars( CleanDigits.data(), CleanDigits.data() + CleanDigits.size(), FloatVal );
    if ( Error == std::errc{} )
    {
        const double Product       = FloatVal * Unit.FloatMultiplier;
        const auto [EndPtr, ToErr] = std::to_chars( OutBuf, OutBuf + OutCap, Product );
        char *OutPtr               = EndPtr;
        const std::string_view S( OutBuf, static_cast<std::size_t>( OutPtr - OutBuf ) );
        if ( S.find( '.' ) == std::string_view::npos and S.find( 'e' ) == std::string_view::npos and
             S.find( 'E' ) == std::string_view::npos and static_cast<std::size_t>( OutPtr - OutBuf ) + 2 < OutCap )
        {
            *OutPtr++ = '.';
            *OutPtr++ = '0';
        }
        Res.WrittenSize = static_cast<std::size_t>( OutPtr - OutBuf );
        Res.bFloat      = true;
    }

    return Res;
}

} // namespace Volt::Frontend
