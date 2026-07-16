#include "Volt/Core/Diagnostics/DiagEngine.hpp"

#include "Volt/Core/Diagnostics/SourceManager.hpp"

#include <cstdint>
#include <ostream>
#include <string>

namespace Volt
{

    namespace Core
    {

        namespace
        {

            void RenderOne( const SourceManager& Sources, std::ostream& Out, ESeverity Severity, const SourceRange& Range, const std::string& Message )
            {
                const LineColumn Where = Sources.Resolve( Range.File, Range.Begin );

                Out << Sources.PathOf( Range.File ) << ':' << Where.Line << ':' << Where.Column << ": " << SeverityName( Severity ) << ": " << Message << '\n';

                const std::string_view Line = Sources.LineText( Range.File, Range.Begin );
                Out << "    " << Line << '\n';

                // Caret under the range start, spanning the range within the line.
                Out << "    ";
                for ( std::uint32_t Column = 1; Column < Where.Column; ++Column )
                {
                    Out << ' ';
                }

                const std::uint32_t Width = Range.Length() == 0 ? 1 : Range.Length();
                Out << '^';
                for ( std::uint32_t Index = 1; Index < Width; ++Index )
                {
                    Out << '~';
                }
                Out << '\n';
            }

        }

        void DiagEngine::Render( const SourceManager& Sources, std::ostream& Out ) const
        {
            const std::scoped_lock Guard{ Mutex };

            for ( const Diagnostic& Diag : Store )
            {
                RenderOne( Sources, Out, Diag.Severity, Diag.Range, Diag.Message );
                for ( const DiagnosticNote& Note : Diag.Notes )
                {
                    RenderOne( Sources, Out, ESeverity::Note, Note.Range, Note.Message );
                }
            }
        }

    }

}
