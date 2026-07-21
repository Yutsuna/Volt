#pragma once

#include "Volt/Core/Diagnostics/SourceLocation.hpp"

constexpr std::uint32_t Volt::Core::SourceRange::Length () const noexcept
{
    return End - Begin;
}

constexpr Volt::Core::SourceRange Volt::Core::SourceRange::Merge ( SourceRange Lhs, SourceRange Rhs ) noexcept
{
    return {
        .File = Lhs.File, .Begin = Lhs.Begin < Rhs.Begin ? Lhs.Begin : Rhs.Begin, .End = Lhs.End > Rhs.End ? Lhs.End : Rhs.End };
}
