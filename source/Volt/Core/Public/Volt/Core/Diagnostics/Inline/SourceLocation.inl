#pragma once

#include "Volt/Core/Diagnostics/SourceLocation.hpp"

constexpr std::uint32_t Volt::Core::SourceRange::Length () const noexcept
{
    return End - Begin;
}

// A range collapsed onto its first byte. A diagnostic about a declaration as a
// whole should still point at where it starts, not underline its entire body.
constexpr Volt::Core::SourceRange Volt::Core::SourceRange::Head () const noexcept
{
    return { .File = File, .Begin = Begin, .End = Begin };
}

constexpr Volt::Core::SourceRange Volt::Core::SourceRange::Merge ( SourceRange Lhs, SourceRange Rhs ) noexcept
{
    return {
        .File = Lhs.File, .Begin = Lhs.Begin < Rhs.Begin ? Lhs.Begin : Rhs.Begin, .End = Lhs.End > Rhs.End ? Lhs.End : Rhs.End };
}
