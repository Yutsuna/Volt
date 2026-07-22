#pragma once

#include "Core_export.hpp"
#include "Volt/Core/Diagnostics/SourceLocation.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Core
{

    /**
     * @class SourceManager
     * @brief OWns the text of every source file and maps FileId <-> path/content.
     * @details Files are registered up-front (single-threaded)
     *          After that all lookups are const safe to call from parallel parse threads.
     */
    class CORE_EXPORT SourceManager
    {

    public:

        /**
         * @struct FileEntry
         * @brief Represents a single source file
         */
        struct FileEntry
        {

            std::string Path;
            std::string Text;
            std::vector<std::uint32_t> LineStarts; //<< byte offset of each line
        };

        /** @brief Adds a new source file to the manager */
        [[nodiscard]] FileId AddFile ( std::string Path, std::string Text );

        /** @brief Gets the path of a source file */
        [[nodiscard]] std::string_view PathOf ( FileId File ) const;

        /** @brief Gets the text of a source file */
        [[nodiscard]] std::string_view TextOf ( FileId File ) const;

        /** @brief Resolves a byte offset to a 1-based line/column pair */
        [[nodiscard]] LineColumn Resolve ( FileId File, std::uint32_t Offset ) const;

        /** @brief Gets the text of the line containing the given offset */
        [[nodiscard]] std::string_view LineText ( FileId File, std::uint32_t Offset ) const;

        /** @brief Gets the number of files */
        [[nodiscard]] std::size_t FileCount () const noexcept;

        /**
         * @brief Whether File actually indexes a registered file.
         *
         * Every other accessor indexes `Files` unchecked, so a default or
         * foreign FileId is a wild read. A diagnostic may legitimately carry
         * no location (a pass that knows a fact but not where it came from),
         * so renderers must ask this before resolving.
         */
        [[nodiscard]] bool IsValidFile ( FileId File ) const noexcept;

    private:

        [[nodiscard]] const FileEntry &EntryOf ( FileId File ) const;

        std::vector<FileEntry> Files;
    };

} // namespace Core

} // namespace Volt
