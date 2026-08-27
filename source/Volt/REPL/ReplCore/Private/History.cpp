// History.cpp — see History.hpp.

#include "Volt/ReplCore/History.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace
{

[[nodiscard]] std::string_view Trim ( std::string_view Text )
{
    constexpr std::string_view Blank = " \t\r\n";
    const std::size_t First          = Text.find_first_not_of( Blank );
    if ( First == std::string_view::npos )
    {
        return {};
    }
    return Text.substr( First, Text.find_last_not_of( Blank ) + 1 - First );
}

} // namespace

void Volt::Repl::History::Add ( std::string Statement )
{
    if ( Trim( Statement ).empty() )
    {
        return;
    }
    if ( not Entries.empty() and Entries.back() == Statement )
    {
        return;
    }

    Entries.push_back( std::move( Statement ) );
    if ( Entries.size() > Limit )
    {
        // One at a time, because that is how they arrive: the vector shifts
        // once per line past the limit rather than in a batch nobody would
        // notice either.
        Entries.erase( Entries.begin() );
    }
}

std::string_view Volt::Repl::History::At ( const std::size_t Index ) const
{
    return Index < Entries.size() ? std::string_view( Entries[Index] ) : std::string_view{};
}

std::optional<std::size_t> Volt::Repl::History::SearchBackwards ( const std::string_view Needle, const std::size_t From ) const
{
    if ( Needle.empty() or Entries.empty() )
    {
        return std::nullopt;
    }

    for ( std::size_t Index = std::min( From + 1, Entries.size() ); Index > 0; --Index )
    {
        if ( Entries[Index - 1].find( Needle ) != std::string::npos )
        {
            return Index - 1;
        }
    }
    return std::nullopt;
}
