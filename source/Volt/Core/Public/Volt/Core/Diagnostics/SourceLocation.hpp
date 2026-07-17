#pragma once

#include "Volt/Core/Support/Id.hpp"

#include <cstdint>

namespace Volt
{

namespace Core
{

    struct FileTag
    {
    };

    /// Handle for a file registered with the SourceManager.
    using FileId = TypedId<FileTag>;

    /// Half-open byte range [Begin, End) inside a single source file. AST
    /// nodes and tokens carry these; line/column is derived on demand by
    /// the SourceManager so hot paths stay offset-only.
    struct SourceRange
    {

        FileId File;
        std::uint32_t Begin = 0;
        std::uint32_t End   = 0;

        [[nodiscard]] constexpr std::uint32_t Length () const
        {
            return End - Begin;
        }

        [[nodiscard]] static constexpr SourceRange Merge ( SourceRange Lhs, SourceRange Rhs )
        {
            return SourceRange{ Lhs.File, Lhs.Begin < Rhs.Begin ? Lhs.Begin : Rhs.Begin, Lhs.End > Rhs.End ? Lhs.End : Rhs.End };
        }
    };

    /// Human-facing 1-based position resolved from a byte offset.
    struct LineColumn
    {

        std::uint32_t Line   = 1;
        std::uint32_t Column = 1;
    };

} // namespace Core

} // namespace Volt
