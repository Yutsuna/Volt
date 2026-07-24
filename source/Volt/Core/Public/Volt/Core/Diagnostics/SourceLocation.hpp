#pragma once

#include "Volt/Core/Support/Id.hpp"

#include <cstdint>

namespace Volt
{

namespace Core
{

    /**
     * @tag FileTag
     * @brief Tag type for FileId, used to differentiate it from other TypedId types
     */
    struct FileTag
    {
    };

    /**
     * @typedef FileId
     * @brief Handle for a file registered with the SourceManager.
     */
    using FileId = TypedId<FileTag>;

    /**
     * @struct SourceRange
     * @brief Half-open byte range inside a single source file.
     */
    struct SourceRange
    {

        FileId File;
        std::uint32_t Begin = 0;
        std::uint32_t End   = 0;

        [[nodiscard]] constexpr std::uint32_t Length () const noexcept;
        [[nodiscard]] constexpr SourceRange Head () const noexcept;
        [[nodiscard]] static constexpr SourceRange Merge ( SourceRange Lhs, SourceRange Rhs ) noexcept;
    };

    /**
     * @struct LineColumn
     * @brief Human-facing 1-based position resolved from a byte offset.
     */
    struct LineColumn
    {

        std::uint32_t Line   = 1;
        std::uint32_t Column = 1;
    };

} // namespace Core

} // namespace Volt

#include "Inline/SourceLocation.inl"
