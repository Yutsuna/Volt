#pragma once

#include "Volt/Core/Diagnostics/Diagnostic.hpp"

constexpr std::string_view Volt::Core::SeverityName ( ESeverity Severity ) noexcept
{
    switch ( Severity )
    {
    case ESeverity::Note:
        return "note";
    case ESeverity::Warning:
        return "warning";
    case ESeverity::Error:
        return "error";
    }
    return "diagnostic";
}
