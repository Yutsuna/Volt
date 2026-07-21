#pragma once

#include "Volt/Core/Diagnostics/SourceLocation.hpp"

#include <string>
#include <vector>

namespace Volt
{

namespace Core
{

    /**
     * @enum ESeverity
     * @brief Represents the severity level of a diagnostic message.
     */
    enum class ESeverity : uint8_t
    {

        Note,
        Warning,
        Error,
    };

    /**
     * @brief Returns the string representation of the given severity level.
     * @param Severity The severity level to convert to a string.
     * @return A string view representing the severity level.
     */
    [[nodiscard]] constexpr std::string_view SeverityName ( ESeverity Severity ) noexcept;

    /**
     * @struct DiagnosticNote
     * @brief Secondary annotation associated with a diagnostic message.
     */
    struct DiagnosticNote
    {

        SourceRange Range;
        std::string Message;
    };

    /**
     * @struct Diagnostic
     * @brief Represents a diagnostic message
     */
    struct Diagnostic
    {

        ESeverity Severity = ESeverity::Error;
        SourceRange Range;
        std::string Message;
        std::vector<DiagnosticNote> Notes;

        /**
         * @brief Adds a note to the diagnostic.
         * @param NoteRange The source range associated with the note.
         * @param NoteMessage The message for the note.
         * @return A reference to the current Diagnostic object.
         */
        [[nodiscard]] Diagnostic &AddNote ( SourceRange NoteRange, std::string NoteMessage );
    };

} // namespace Core

} // namespace Volt

#include "Inline/Diagnostic.inl"
