#pragma once

#include "Volt/Core/Diagnostics/SourceLocation.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

    namespace Core
    {

        /// Owns the text of every source file and maps FileId <-> path/content.
        /// Files are registered up-front (single-threaded); after that all
        /// lookups are const and safe to call from parallel parse threads.
        class SourceManager
        {

        public:

            struct FileEntry
            {

                std::string                Path;
                std::string                Text;
                std::vector<std::uint32_t> LineStarts; // byte offset of each line
            };

            [[nodiscard]] FileId AddFile( std::string Path, std::string Text );

            [[nodiscard]] std::string_view PathOf( FileId File ) const;
            [[nodiscard]] std::string_view TextOf( FileId File ) const;

            /// Resolve a byte offset to a 1-based line/column pair.
            [[nodiscard]] LineColumn Resolve( FileId File, std::uint32_t Offset ) const;

            /// The raw text of the line containing Offset (without the newline).
            [[nodiscard]] std::string_view LineText( FileId File, std::uint32_t Offset ) const;

            [[nodiscard]] std::size_t FileCount() const
            {
                return Files.size();
            }

        private:

            [[nodiscard]] const FileEntry& EntryOf( FileId File ) const;

            std::vector<FileEntry> Files;
        };

    }

}
