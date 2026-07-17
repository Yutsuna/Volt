#include "Volt/Core/Diagnostics/SourceManager.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

namespace Volt
{

namespace Core
{

    FileId SourceManager::AddFile ( std::string Path, std::string Text )
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

    const SourceManager::FileEntry &SourceManager::EntryOf ( FileId File ) const
    {
        return Files[File.Value];
    }

    std::string_view SourceManager::PathOf ( FileId File ) const
    {
        return EntryOf( File ).Path;
    }

    std::string_view SourceManager::TextOf ( FileId File ) const
    {
        return EntryOf( File ).Text;
    }

    LineColumn SourceManager::Resolve ( FileId File, std::uint32_t Offset ) const
    {
        const FileEntry &Entry = EntryOf( File );

        // Last line whose start offset is <= Offset.
        const auto It    = std::upper_bound( Entry.LineStarts.begin(), Entry.LineStarts.end(), Offset );
        const auto Index = static_cast<std::uint32_t>( std::distance( Entry.LineStarts.begin(), It ) - 1 );

        LineColumn Result;
        Result.Line   = Index + 1;
        Result.Column = ( Offset - Entry.LineStarts[Index] ) + 1;
        return Result;
    }

    std::string_view SourceManager::LineText ( FileId File, std::uint32_t Offset ) const
    {
        const FileEntry &Entry = EntryOf( File );

        const auto It    = std::upper_bound( Entry.LineStarts.begin(), Entry.LineStarts.end(), Offset );
        const auto Index = static_cast<std::size_t>( std::distance( Entry.LineStarts.begin(), It ) - 1 );

        const std::size_t Begin = Entry.LineStarts[Index];
        std::size_t End         = ( Index + 1 < Entry.LineStarts.size() ) ? Entry.LineStarts[Index + 1] : Entry.Text.size();

        // Trim the trailing newline so callers get just the visible line.
        while ( End > Begin && ( Entry.Text[End - 1] == '\n' || Entry.Text[End - 1] == '\r' ) )
        {
            --End;
        }

        return std::string_view{ Entry.Text }.substr( Begin, End - Begin );
    }

} // namespace Core

} // namespace Volt
