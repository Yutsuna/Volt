#include "Volt/Core/Diagnostics/SourceManager.hpp"
#include "Volt/Core/Diagnostics/SourceLocation.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

/**
 * Private helpers
 */

namespace
{

inline void TrimTrailingNewlines ( const std::string &Text, const std::size_t Begin, std::size_t *OutEnd ) noexcept
{
    while ( *OutEnd > Begin and ( Text[*OutEnd - 1] == '\n' or Text[*OutEnd - 1] == '\r' ) )
    {
        --*OutEnd;
    }
}

} // namespace

/**
 * Public
 */

/**
 * SourceManager
 */

Volt::Core::FileId Volt::Core::SourceManager::AddFile ( std::string Path, std::string Text )
{
    const auto Index = static_cast<FileId::ValueType>( Files.size() );

    FileEntry Entry;
    Entry.Path = std::move( Path );
    Entry.Text = std::move( Text );

    Entry.LineStarts.push_back( 0 );
    for ( std::size_t Offset = 0; Offset < Entry.Text.size(); ++Offset )
    {
        if ( Entry.Text[Offset] == '\n' )
        {
            Entry.LineStarts.push_back( static_cast<std::uint32_t>( Offset + 1 ) );
        }
    }

    Files.push_back( std::move( Entry ) );
    return FileId{ Index };
}

const Volt::Core::SourceManager::FileEntry &Volt::Core::SourceManager::EntryOf ( FileId File ) const
{
    return Files[File.Value];
}

std::string_view Volt::Core::SourceManager::PathOf ( FileId File ) const
{
    return EntryOf( File ).Path;
}

std::string_view Volt::Core::SourceManager::TextOf ( FileId File ) const
{
    return EntryOf( File ).Text;
}

Volt::Core::LineColumn Volt::Core::SourceManager::Resolve ( FileId File, std::uint32_t Offset ) const
{
    const FileEntry &Entry = EntryOf( File );

    const auto It    = std::ranges::upper_bound( Entry.LineStarts, Offset );
    const auto Index = static_cast<std::uint32_t>( std::distance( Entry.LineStarts.begin(), It ) - 1 );

    LineColumn Result;
    Result.Line   = Index + 1;
    Result.Column = ( Offset - Entry.LineStarts[Index] ) + 1;
    return Result;
}

std::string_view Volt::Core::SourceManager::LineText ( FileId File, std::uint32_t Offset ) const
{
    const FileEntry &Entry = EntryOf( File );
    const auto It          = std::ranges::upper_bound( Entry.LineStarts, Offset );
    const auto Index       = static_cast<std::size_t>( std::distance( Entry.LineStarts.begin(), It ) - 1 );

    const std::size_t Begin = Entry.LineStarts[Index];
    std::size_t End         = ( Index + 1 < Entry.LineStarts.size() ) ? Entry.LineStarts[Index + 1] : Entry.Text.size();

    TrimTrailingNewlines( Entry.Text, Begin, &End );

    return std::string_view{ Entry.Text }.substr( Begin, End - Begin );
}

std::size_t Volt::Core::SourceManager::FileCount () const noexcept
{
    return Files.size();
}
