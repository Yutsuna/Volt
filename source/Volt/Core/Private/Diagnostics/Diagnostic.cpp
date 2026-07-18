#include "Volt/Core/Diagnostics/Diagnostic.hpp"
#include "Volt/Core/Diagnostics/SourceLocation.hpp"

Volt::Core::Diagnostic &Volt::Core::Diagnostic::AddNote ( Volt::Core::SourceRange NoteRange, std::string NoteMessage )
{
    Notes.push_back( DiagnosticNote{ .Range = NoteRange, .Message = std::move( NoteMessage ) } );
    return *this;
}
