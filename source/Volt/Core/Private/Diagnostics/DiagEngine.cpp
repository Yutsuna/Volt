#include "Volt/Core/Diagnostics/DiagEngine.hpp"

#include "Volt/Core/Diagnostics/Diagnostic.hpp"
#include "Volt/Core/Diagnostics/SourceManager.hpp"

#include <cstdint>
#include <iterator>
#include <ostream>
#include <string>

namespace
{
void RenderOne ( const Volt::Core::SourceManager &Sources,
                 std::ostream &Out,
                 Volt::Core::ESeverity Severity,
                 const Volt::Core::SourceRange &Range,
                 const std::string &Message );
} // namespace

/**
 * Public
 */

/**
 * DiagEngine::Bag
 */

Volt::Core::Diagnostic &Volt::Core::DiagEngine::Bag::Report ( Diagnostic Diag )
{
    if ( Diag.Severity == ESeverity::Error )
    {
        ++ErrorCount;
    }
    return Items.emplace_back( std::move( Diag ) );
}

Volt::Core::Diagnostic &Volt::Core::DiagEngine::Bag::Error ( SourceRange Range, std::string Message )
{
    return Report( Diagnostic{ .Severity = ESeverity::Error, .Range = Range, .Message = std::move( Message ), .Notes = {} } );
}

Volt::Core::Diagnostic &Volt::Core::DiagEngine::Bag::Warning ( SourceRange Range, std::string Message )
{
    return Report( Diagnostic{ .Severity = ESeverity::Warning, .Range = Range, .Message = std::move( Message ), .Notes = {} } );
}

[[nodiscard]] std::size_t Volt::Core::DiagEngine::Bag::Errors () const noexcept
{
    return ErrorCount;
}

/**
 * DiagEngine
 */

Volt::Core::DiagEngine::Bag Volt::Core::DiagEngine::MakeBag () noexcept
{
    return {};
}

void Volt::Core::DiagEngine::Merge ( Bag &&Local )
{
    const std::scoped_lock Guard{ Mutex };

    ErrorCount += Local.ErrorCount;
    auto &&Items = std::move( Local ).Items;
    Store.insert( Store.end(), std::make_move_iterator( Items.begin() ), std::make_move_iterator( Items.end() ) );
}

Volt::Core::Diagnostic &Volt::Core::DiagEngine::Report ( Diagnostic Diag )
{
    const std::scoped_lock Guard{ Mutex };

    if ( Diag.Severity == ESeverity::Error )
    {
        ++ErrorCount;
    }
    return Store.emplace_back( std::move( Diag ) );
}

void Volt::Core::DiagEngine::Render ( const SourceManager &Sources, std::ostream &Out ) const
{
    const std::scoped_lock Guard{ Mutex };

    for ( const Diagnostic &Diag : Store )
    {
        RenderOne( Sources, Out, Diag.Severity, Diag.Range, Diag.Message );
        for ( const DiagnosticNote &Note : Diag.Notes )
        {
            RenderOne( Sources, Out, ESeverity::Note, Note.Range, Note.Message );
        }
    }
}

std::size_t Volt::Core::DiagEngine::ErrorTotal () const noexcept
{
    const std::scoped_lock Guard{ Mutex };
    return ErrorCount;
}

bool Volt::Core::DiagEngine::HasErrors () const noexcept
{
    return ErrorTotal() != 0;
}

std::size_t Volt::Core::DiagEngine::Count () const noexcept
{
    const std::scoped_lock Guard{ Mutex };
    return Store.size();
}

/**
 * Private helpers
 */

namespace
{
void RenderOne ( const Volt::Core::SourceManager &Sources,
                 std::ostream &Out,
                 Volt::Core::ESeverity Severity,
                 const Volt::Core::SourceRange &Range,
                 const std::string &Message )
{
    // A location-less diagnostic is legitimate: a pass may know a fact
    // without a source range to pin it to. Resolving it would index Files
    // out of bounds, so degrade to the bare message instead.
    if ( not Sources.IsValidFile( Range.File ) )
    {
        Out << SeverityName( Severity ) << ": " << Message << '\n';
        return;
    }

    const Volt::Core::LineColumn Where = Sources.Resolve( Range.File, Range.Begin );

    Out << Sources.PathOf( Range.File ) << ':' << Where.Line << ':' << Where.Column << ": " << SeverityName( Severity ) << ": "
        << Message << '\n';

    const std::string_view Line = Sources.LineText( Range.File, Range.Begin );
    Out << "    " << Line << '\n';

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

} // namespace
