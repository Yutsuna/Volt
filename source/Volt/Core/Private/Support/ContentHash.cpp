#include "Volt/Core/Support/ContentHash.hpp"

#include <array>
#include <fstream>
#include <ios>

std::optional<std::uint64_t> Volt::Core::HashFile ( const std::filesystem::path &Path, std::uint64_t Seed )
{
    std::ifstream Stream( Path, std::ios::binary );
    if ( not Stream )
    {
        return std::nullopt;
    }

    std::uint64_t State = Seed;
    std::array<char, 65536> Buffer{};
    while ( Stream.read( Buffer.data(), static_cast<std::streamsize>( Buffer.size() ) ) or Stream.gcount() > 0 )
    {
        State = HashBytes( std::string_view{ Buffer.data(), static_cast<std::size_t>( Stream.gcount() ) }, State );
    }
    return State;
}

std::optional<std::uint64_t> Volt::Core::HashFileTree ( const std::vector<std::filesystem::path> &SortedFiles,
                                                        std::uint64_t Seed )
{
    std::uint64_t State = Seed;
    for ( const std::filesystem::path &Path : SortedFiles )
    {
        State                                       = CombineHash( State, Path.string() );
        const std::optional<std::uint64_t> FileHash = HashFile( Path, State );
        if ( not FileHash.has_value() )
        {
            return std::nullopt;
        }
        State = *FileHash;
    }
    return State;
}

std::string Volt::Core::ToHex ( std::uint64_t Value )
{
    static constexpr std::string_view Digits = "0123456789abcdef";
    std::string Result( 16, '0' );
    for ( std::size_t Index = 0; Index < 16; ++Index )
    {
        const std::uint64_t Nibble = ( Value >> ( 4 * ( 15 - Index ) ) ) & 0xFULL;
        Result[Index]              = Digits[Nibble];
    }
    return Result;
}
