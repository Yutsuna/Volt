#pragma once

#include "Volt/Core/Diagnostics/SourceLocation.hpp"

#include <string>
#include <vector>

namespace Volt
{

    namespace Core
    {

        enum class ESeverity
        {

            Note,
            Warning,
            Error,
        };

        [[nodiscard]] constexpr std::string_view SeverityName( ESeverity Severity )
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

        /// A secondary annotation attached to a primary diagnostic.
        struct DiagnosticNote
        {

            SourceRange Range;
            std::string Message;
        };

        /// One diagnostic: severity, where, what, and optional notes. Kept as a
        /// plain value so DiagEngine can buffer per-thread and merge cheaply.
        struct Diagnostic
        {

            ESeverity                   Severity = ESeverity::Error;
            SourceRange                 Range;
            std::string                 Message;
            std::vector<DiagnosticNote> Notes;

            [[nodiscard]] Diagnostic& AddNote( SourceRange NoteRange, std::string NoteMessage )
            {
                Notes.push_back( DiagnosticNote{ NoteRange, std::move( NoteMessage ) } );
                return *this;
            }
        };

    }

}
