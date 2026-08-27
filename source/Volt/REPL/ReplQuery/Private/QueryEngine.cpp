// QueryEngine.cpp — see QueryEngine.hpp.
//
// Two halves. Parse turns a line into a Command by consulting one table; Run
// turns a Command into a Document by asking the session a question and
// arranging the answer. Neither half computes anything the session could have
// been asked for directly — a formatter that derived a fact would be a second
// place for that fact to be wrong.

#include "Volt/ReplQuery/QueryEngine.hpp"

#include "Volt/ReplDoc/Table.hpp"
#include "Volt/ReplSyntax/Highlighter.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace Volt::Repl;

constexpr std::array<Query::Builtin, 12> Table = {
    Query::Builtin{ Query::EBuiltin::Help, "help", "", "This table", false },
    Query::Builtin{ Query::EBuiltin::Type, "type", "<expr>", "The type of an expression, without evaluating it", false },
    Query::Builtin{ Query::EBuiltin::Layout, "layout", "<type|expr>", "Size, alignment and fields in memory", false },
    Query::Builtin{ Query::EBuiltin::Ir, "ir", "[expr]", "The LLVM IR of the last line, or of an expression", true },
    Query::Builtin{ Query::EBuiltin::Asm, "asm", "<name>", "The machine code a function materialised as", true },
    Query::Builtin{ Query::EBuiltin::Src, "src", "<name>", "The source a declaration was written as", true },
    Query::Builtin{ Query::EBuiltin::Doc, "doc", "<name>", "The block comment above a declaration", true },
    Query::Builtin{ Query::EBuiltin::Bench, "bench", "[n] <expr>", "Run an expression n times and time it", false },
    Query::Builtin{ Query::EBuiltin::History, "history", "", "Every line this session has evaluated", true },
    Query::Builtin{ Query::EBuiltin::Vars, "vars", "", "Every variable declared at this prompt", false },
    Query::Builtin{ Query::EBuiltin::Reset, "reset", "", "Throw the session away and start a fresh one", false },
    Query::Builtin{ Query::EBuiltin::Exit, "exit", "", "Leave (so does ^D)", false },
};

[[nodiscard]] std::string_view Trim ( std::string_view Text )
{
    constexpr std::string_view Blank = " \t\r\n";
    const std::size_t First          = Text.find_first_not_of( Blank );
    if ( First == std::string_view::npos )
    {
        return {};
    }
    return Text.substr( First, Text.find_last_not_of( Blank ) + 1 - First );
}

// Nanoseconds as something a human reads at a glance: three significant
// figures and the unit that gives them.
[[nodiscard]] std::string Duration ( const double Nanos )
{
    struct Unit
    {

        double Scale;
        std::string_view Suffix;
    };

    constexpr std::array<Unit, 4> Units = { Unit{ 1.0, "ns" }, Unit{ 1e3, "us" }, Unit{ 1e6, "ms" }, Unit{ 1e9, "s" } };

    const Unit *Chosen = &Units[0];
    for ( const Unit &Each : Units )
    {
        if ( Nanos >= Each.Scale )
        {
            Chosen = &Each;
        }
    }

    const double Value = Nanos / Chosen->Scale;
    std::string Text   = std::to_string( Value );

    // Three significant figures: `123`, `12.3`, `1.23`.
    const std::size_t Point = Text.find( '.' );
    if ( Point != std::string::npos )
    {
        const std::size_t Keep = Point >= 3 ? Point : 4;
        Text.resize( std::min( Text.size(), Keep ) );
        if ( not Text.empty() and Text.back() == '.' )
        {
            Text.pop_back();
        }
    }
    return Text + std::string( Chosen->Suffix );
}

// `:bench 500 expr` — a leading run of digits is the iteration count, and
// anything else is the whole expression.
struct BenchArgs
{

    std::size_t Iterations = 0;
    std::string_view Expression;
};

[[nodiscard]] BenchArgs SplitBench ( const std::string_view Argument, const std::size_t Fallback )
{
    const std::string_view Rest = Trim( Argument );

    std::size_t Digits = 0;
    while ( Digits < Rest.size() and Rest[Digits] >= '0' and Rest[Digits] <= '9' )
    {
        ++Digits;
    }

    // Digits alone are an expression, not a count: `:bench 42` measures the
    // literal, which is a fair thing to ask for.
    if ( Digits == 0 or Digits == Rest.size() )
    {
        return BenchArgs{ .Iterations = Fallback, .Expression = Rest };
    }

    // `:bench 1 + 2` is a sum, not one iteration of `+ 2`. A count is a whole
    // word followed by something that can *begin* an expression, and an
    // operator cannot — so the leading number belongs to the expression
    // whenever one follows it.
    constexpr std::string_view Continues = "+-*/%<>=&|^.,)]}?:";
    const std::string_view After         = Trim( Rest.substr( Digits ) );
    if ( Rest[Digits] != ' ' and Rest[Digits] != '\t' )
    {
        return BenchArgs{ .Iterations = Fallback, .Expression = Rest };
    }
    if ( After.empty() or Continues.find( After.front() ) != std::string_view::npos )
    {
        return BenchArgs{ .Iterations = Fallback, .Expression = Rest };
    }

    std::size_t Count = 0;
    for ( const char Digit : Rest.substr( 0, Digits ) )
    {
        Count = ( Count * 10 ) + static_cast<std::size_t>( Digit - '0' );
    }
    return BenchArgs{ .Iterations = std::max<std::size_t>( 1, Count ), .Expression = After };
}

} // namespace

std::span<const Volt::Repl::Query::Builtin> Volt::Repl::Query::Builtins ()
{
    return Table;
}

Volt::Repl::Query::Command Volt::Repl::Query::Parse ( const std::string_view Line )
{
    Command Out;

    const std::string_view Text = Trim( Line );
    if ( Text.size() < 2 or Text.front() != ':' )
    {
        return Out;
    }

    const std::string_view Rest = Text.substr( 1 );
    const std::size_t Break     = Rest.find_first_of( " \t" );
    const std::string_view Word = Rest.substr( 0, Break );

    for ( const Builtin &Each : Table )
    {
        if ( Each.Name != Word )
        {
            continue;
        }

        Out.Kind     = Each.Kind;
        Out.Name     = std::string( Word );
        Out.Argument = Break == std::string_view::npos ? std::string{} : std::string( Trim( Rest.substr( Break ) ) );
        return Out;
    }

    // A colon-word nobody claimed is a Volt symbol literal, and evaluating it
    // is the right answer.
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::Failure ( std::string Message ) const
{
    Result Out;
    Out.Body.PushText( std::move( Message ), Doc::RoleColor( Theme, Doc::EPaletteRole::Error ) );
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::Run ( const Command &What, const std::span<const std::string> History )
{
    switch ( What.Kind )
    {
    case EBuiltin::Help:
        return Help();
    case EBuiltin::Type:
        return Type( What.Argument );
    case EBuiltin::Layout:
        return Layout( What.Argument );
    case EBuiltin::Ir:
        return Ir( What.Argument );
    case EBuiltin::Asm:
        return Asm( What.Argument );
    case EBuiltin::Src:
        return Src( What.Argument );
    case EBuiltin::Doc:
        return DocFor( What.Argument );
    case EBuiltin::Bench:
        return Bench( What.Argument );
    case EBuiltin::History:
        return this->History( History );
    case EBuiltin::Vars:
        return Vars();
    case EBuiltin::Reset:
        return Reset();
    case EBuiltin::Exit:
    {
        Result Out;
        Out.bOk   = true;
        Out.bExit = true;
        return Out;
    }
    case EBuiltin::None:
        break;
    }
    return Failure( "repl: that is not a builtin" );
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::Help () const
{
    Doc::Table Grid;
    Grid.Headers = { Doc::Cell{ "Command" }, Doc::Cell{ "Argument" }, Doc::Cell{ "Does" } };

    for ( const Builtin &Each : Table )
    {
        Grid.Rows.push_back( { Doc::Cell{ ":" + std::string( Each.Name ), Doc::EPaletteRole::Keyword },
                               Doc::Cell{ std::string( Each.Argument ), Doc::EPaletteRole::Identifier },
                               Doc::Cell{ std::string( Each.Summary ) } } );
    }

    Result Out;
    Out.bOk  = true;
    Out.Body = Doc::Render( Grid, Theme, Doc::EBorder::Unicode, Columns );
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::Type ( const std::string_view Expression ) const
{
    if ( Trim( Expression ).empty() )
    {
        return Failure( "repl: :type needs an expression" );
    }

    const Evaluator::TypeAnswer Answer = Session.TypeOf( Expression );
    if ( not Answer.bOk )
    {
        Result Out;
        for ( const Doc::Line &Row : Syntax::HighlightVolt( Answer.Diagnostics, Theme ).Lines )
        {
            Out.Body.Push( Row );
        }
        if ( Out.Body.Empty() )
        {
            return Failure( "repl: that has no type" );
        }
        return Out;
    }

    Result Out;
    Out.bOk = true;
    Out.Body.PushText( Answer.Name, Doc::RoleColor( Theme, Doc::EPaletteRole::TypeName ) );
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::Layout ( const std::string_view Name ) const
{
    if ( Trim( Name ).empty() )
    {
        return Failure( "repl: :layout needs a type or an expression" );
    }

    const Evaluator::LayoutAnswer Answer = Session.LayoutOf( Name );
    if ( not Answer.bOk )
    {
        return Failure( Answer.Message );
    }

    Doc::Table Grid;
    Grid.Headers   = { Doc::Cell{ "Field" }, Doc::Cell{ "Type" }, Doc::Cell{ "Offset" }, Doc::Cell{ "Size" },
                       Doc::Cell{ "Align" } };
    Grid.Alignment = { Doc::EAlign::Left, Doc::EAlign::Left, Doc::EAlign::Right, Doc::EAlign::Right, Doc::EAlign::Right };

    for ( const Evaluator::FieldFact &Field : Answer.Fields )
    {
        Grid.Rows.push_back( { Doc::Cell{ Field.Name, Doc::EPaletteRole::Identifier },
                               Doc::Cell{ Field.Type, Doc::EPaletteRole::TypeName },
                               Doc::Cell{ std::to_string( Field.Offset ), Doc::EPaletteRole::Number },
                               Doc::Cell{ std::to_string( Field.Size ), Doc::EPaletteRole::Number },
                               Doc::Cell{ std::to_string( Field.Align ), Doc::EPaletteRole::Number } } );
    }

    Result Out;
    Out.bOk = true;

    Doc::Line Heading;
    Heading.Add( Answer.Type, Doc::RoleColor( Theme, Doc::EPaletteRole::TypeName ) );
    Heading.Add( "  " + Answer.Kind + ", " + std::to_string( Answer.Size ) + " bytes, aligned to " +
                     std::to_string( Answer.Align ),
                 Doc::RoleColor( Theme, Doc::EPaletteRole::ResultType ) );
    Out.Body.Push( std::move( Heading ) );

    if ( not Grid.Rows.empty() )
    {
        Out.Body.Append( Doc::Render( Grid, Theme, Doc::EBorder::Unicode, Columns ) );
    }
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::Ir ( const std::string_view Expression ) const
{
    const Evaluator::TextAnswer Answer = Trim( Expression ).empty() ? Session.LastIr() : Session.IrOf( Expression );
    if ( not Answer.bOk )
    {
        return Failure( Answer.Message );
    }

    Result Out;
    Out.bOk       = true;
    Out.Placement = EPlacement::Panel;
    Out.Title     = Trim( Expression ).empty() ? "ir: the last line" : "ir: " + std::string( Trim( Expression ) );
    Out.Body      = Syntax::HighlightIr( Answer.Text, Theme );
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::Asm ( const std::string_view Name ) const
{
    if ( Trim( Name ).empty() )
    {
        return Failure( "repl: :asm needs a function or a symbol" );
    }

    const Evaluator::TextAnswer Answer = Session.AsmOf( Name, AsmWindow );
    if ( not Answer.bOk )
    {
        return Failure( Answer.Message );
    }

    Result Out;
    Out.bOk       = true;
    Out.Placement = EPlacement::Panel;
    Out.Title     = "asm: " + std::string( Trim( Name ) );
    Out.Body      = Syntax::HighlightAsm( Answer.Text, Theme );
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::Src ( const std::string_view Name ) const
{
    if ( Trim( Name ).empty() )
    {
        return Failure( "repl: :src needs a declaration" );
    }

    const Evaluator::TextAnswer Answer = Session.SourceOf( Name );
    if ( not Answer.bOk )
    {
        return Failure( Answer.Message );
    }

    Result Out;
    Out.bOk       = true;
    Out.Placement = EPlacement::Panel;
    Out.Title     = "src: " + std::string( Trim( Name ) );
    Out.Body      = Syntax::HighlightVolt( Answer.Text, Theme );
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::DocFor ( const std::string_view Name ) const
{
    if ( Trim( Name ).empty() )
    {
        return Failure( "repl: :doc needs a declaration" );
    }

    const Evaluator::TextAnswer Answer = Session.DocOf( Name );
    if ( not Answer.bOk )
    {
        return Failure( Answer.Message );
    }

    // The block arrives with its `#{` / `#}` fence and whatever indentation it
    // was written at. Both are punctuation of the *source*, not of the prose,
    // so both come off before it is shown.
    Result Out;
    Out.bOk       = true;
    Out.Placement = EPlacement::Panel;
    Out.Title     = "doc: " + std::string( Trim( Name ) );

    std::string_view Body = Answer.Text;
    if ( Body.starts_with( "#{" ) )
    {
        Body.remove_prefix( 2 );
    }
    if ( Body.ends_with( "#}" ) )
    {
        Body.remove_suffix( 2 );
    }

    std::size_t Cursor = 0;
    while ( Cursor <= Body.size() )
    {
        const std::size_t Break    = Body.find( '\n', Cursor );
        const std::string_view Row = Body.substr( Cursor, ( Break == std::string_view::npos ? Body.size() : Break ) - Cursor );

        std::string_view Text = Trim( Row );
        // A run of `#` lines carries its comment marker on every row; a block
        // carries it only on the fence, which is already off. Either way the
        // marker is punctuation of the source and not of the prose.
        if ( Text.starts_with( '#' ) )
        {
            Text = Trim( Text.substr( 1 ) );
        }

        if ( not Text.empty() or not Out.Body.Empty() )
        {
            Doc::Line Line;
            // `@brief`, `@details`, `@param name` — the tag is structure and
            // reads as one; what follows it is prose.
            if ( Text.starts_with( '@' ) )
            {
                const std::size_t Space = Text.find( ' ' );
                Line.Add( std::string( Text.substr( 0, Space ) ), Doc::RoleColor( Theme, Doc::EPaletteRole::PanelTitle ) );
                if ( Space != std::string_view::npos )
                {
                    Line.Add( std::string( Text.substr( Space ) ) );
                }
            }
            else
            {
                Line.Add( std::string( Text ) );
            }
            Out.Body.Push( std::move( Line ) );
        }

        if ( Break == std::string_view::npos )
        {
            break;
        }
        Cursor = Break + 1;
    }

    // A trailing blank row is the closing fence's own line, and it is noise.
    while ( not Out.Body.Lines.empty() and Doc::LineWidth( Out.Body.Lines.back() ) == 0 )
    {
        Out.Body.Lines.pop_back();
    }
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::Bench ( const std::string_view Argument ) const
{
    const BenchArgs Split = SplitBench( Argument, DefaultBenchIterations );
    if ( Split.Expression.empty() )
    {
        return Failure( "repl: :bench needs an expression" );
    }

    // How many generations were resident before, so the answer can say the
    // benchmark left none behind. The rule is only worth having if it is
    // observable, and this is where it is observed.
    const std::size_t Before = Session.LiveGenerations();

    const Evaluator::BenchAnswer Answer = Session.Bench( Split.Expression, Split.Iterations );
    if ( not Answer.bOk )
    {
        Result Out;
        if ( not Answer.Diagnostics.empty() )
        {
            Out.Body = Syntax::HighlightVolt( Answer.Diagnostics, Theme );
        }
        Out.Body.PushText( Answer.Message, Doc::RoleColor( Theme, Doc::EPaletteRole::Error ) );
        return Out;
    }

    const std::size_t After = Session.LiveGenerations();

    Doc::Table Grid;
    Grid.Headers   = { Doc::Cell{ "Runs" }, Doc::Cell{ "Total" }, Doc::Cell{ "Mean" }, Doc::Cell{ "Best" },
                       Doc::Cell{ "Generations" } };
    Grid.Alignment = { Doc::EAlign::Right, Doc::EAlign::Right, Doc::EAlign::Right, Doc::EAlign::Right, Doc::EAlign::Right };

    const double Total = static_cast<double>( Answer.TotalNanos );
    const double Mean  = Answer.Iterations == 0 ? 0.0 : Total / static_cast<double>( Answer.Iterations );

    Grid.Rows.push_back( { Doc::Cell{ std::to_string( Answer.Iterations ), Doc::EPaletteRole::Number },
                           Doc::Cell{ Duration( Total ), Doc::EPaletteRole::Number },
                           Doc::Cell{ Duration( Mean ), Doc::EPaletteRole::Number },
                           Doc::Cell{ Duration( static_cast<double>( Answer.BestNanos ) ), Doc::EPaletteRole::Number },
                           Doc::Cell{ std::to_string( Before ) + " -> " + std::to_string( After ),
                                      Before == After ? Doc::EPaletteRole::ResultType : Doc::EPaletteRole::Error } } );

    Result Out;
    Out.bOk  = true;
    Out.Body = Doc::Render( Grid, Theme, Doc::EBorder::Unicode, Columns );
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::History ( const std::span<const std::string> Lines ) const
{
    Result Out;
    Out.bOk       = true;
    Out.Placement = EPlacement::Panel;
    Out.Title     = "history";

    if ( Lines.empty() )
    {
        Out.Body.PushText( "nothing yet", Doc::RoleColor( Theme, Doc::EPaletteRole::ResultType ) );
        return Out;
    }

    const std::size_t Width = std::to_string( Lines.size() ).size();
    for ( std::size_t Index = 0; Index < Lines.size(); ++Index )
    {
        const std::string Number = std::to_string( Index + 1 );

        Doc::Line Row;
        Row.Add( std::string( Width - Number.size(), ' ' ) + Number + "  ",
                 Doc::RoleColor( Theme, Doc::EPaletteRole::ResultType ) );
        for ( const Doc::Span &Piece : Syntax::HighlightVoltLine( Lines[Index], Theme ).Spans )
        {
            Row.Spans.push_back( Piece );
        }
        Out.Body.Push( std::move( Row ) );
    }
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::Vars () const
{
    const std::vector<Evaluator::VariableFact> Known = Session.Variables();
    if ( Known.empty() )
    {
        Result Empty;
        Empty.bOk = true;
        Empty.Body.PushText( "no variables yet", Doc::RoleColor( Theme, Doc::EPaletteRole::ResultType ) );
        return Empty;
    }

    Doc::Table Grid;
    Grid.Headers = { Doc::Cell{ "Name" }, Doc::Cell{ "Type" } };
    for ( const Evaluator::VariableFact &Var : Known )
    {
        Grid.Rows.push_back(
            { Doc::Cell{ Var.Name, Doc::EPaletteRole::Identifier }, Doc::Cell{ Var.Type, Doc::EPaletteRole::TypeName } } );
    }

    Result Out;
    Out.bOk  = true;
    Out.Body = Doc::Render( Grid, Theme, Doc::EBorder::Unicode, Columns );
    return Out;
}

Volt::Repl::Query::Result Volt::Repl::Query::Engine::Reset () const
{
    const std::size_t Before = Session.LiveGenerations();

    std::string Error;
    if ( not Session.Reset( Error ) )
    {
        return Failure( Error.empty() ? "repl: the session could not be restarted" : Error );
    }

    Result Out;
    Out.bOk = true;

    Doc::Line Row;
    Row.Add( "session restarted", Doc::RoleColor( Theme, Doc::EPaletteRole::ResultArrow ) );
    Row.Add( "  generations " + std::to_string( Before ) + " -> " + std::to_string( Session.LiveGenerations() ),
             Doc::RoleColor( Theme, Doc::EPaletteRole::ResultType ) );
    Out.Body.Push( std::move( Row ) );
    return Out;
}
