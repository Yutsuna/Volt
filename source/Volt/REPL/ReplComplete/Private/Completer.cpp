// Completer.cpp — see Completer.hpp.

#include "Volt/ReplComplete/Completer.hpp"

#include "Volt/ReplDoc/Table.hpp"
#include "Volt/ReplQuery/QueryEngine.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace Volt::Repl;

[[nodiscard]] bool IsWordChar ( const char C )
{
    return ( C >= 'a' and C <= 'z' ) or ( C >= 'A' and C <= 'Z' ) or ( C >= '0' and C <= '9' ) or C == '_';
}

// A Volt name may end in `?` or `!`, and those are part of the word for
// completion just as they are for the lexer: `empty?` is one name.
[[nodiscard]] bool IsWordTail ( const char C )
{
    return IsWordChar( C ) or C == '?' or C == '!';
}

// The receiver of a `.` at `Dot`, as source text, or empty when there is
// nothing there to type.
//
// Walks backwards over a call chain, balancing brackets so that
// `users.filter( &.active? ).` yields the whole thing rather than stopping at
// the closing paren. Approximate on purpose: what it cannot make sense of, it
// declines, and a completion that declines costs a keystroke.
[[nodiscard]] std::string_view ReceiverBefore ( const std::string_view Text, const std::size_t Dot )
{
    std::size_t Cursor = Dot;
    std::size_t Depth  = 0;

    while ( Cursor > 0 )
    {
        const char C = Text[Cursor - 1];

        if ( C == ')' or C == ']' or C == '}' )
        {
            ++Depth;
            --Cursor;
            continue;
        }
        if ( C == '(' or C == '[' or C == '{' )
        {
            if ( Depth == 0 )
            {
                break;
            }
            --Depth;
            --Cursor;
            continue;
        }

        if ( Depth > 0 )
        {
            --Cursor;
            continue;
        }

        if ( C == '"' )
        {
            // A string literal is a receiver like any other: `"a,b".` should
            // offer String's members. Scan back to its opening quote.
            std::size_t Open = Cursor - 1;
            while ( Open > 0 and Text[Open - 1] != '"' )
            {
                --Open;
            }
            Cursor = Open == 0 ? 0 : Open - 1;
            continue;
        }

        if ( IsWordTail( C ) or C == '.' or C == '@' )
        {
            --Cursor;
            continue;
        }
        break;
    }

    if ( Depth != 0 or Cursor >= Dot )
    {
        return {};
    }
    return Text.substr( Cursor, Dot - Cursor );
}

[[nodiscard]] std::string LongestCommonPrefix ( const std::vector<Complete::Candidate> &Candidates )
{
    if ( Candidates.empty() )
    {
        return {};
    }

    std::string Prefix = Candidates.front().Text;
    for ( const Complete::Candidate &Each : Candidates )
    {
        std::size_t Shared = 0;
        while ( Shared < Prefix.size() and Shared < Each.Text.size() and Prefix[Shared] == Each.Text[Shared] )
        {
            ++Shared;
        }
        Prefix.resize( Shared );
    }
    return Prefix;
}

void SortAndDedup ( std::vector<Complete::Candidate> &Candidates )
{
    // By kind first — a variable the user declared beats a stdlib type of the
    // same spelling, because the near thing is what was meant — then
    // alphabetically inside a kind.
    std::stable_sort( Candidates.begin(), Candidates.end(),
                      [] ( const Complete::Candidate &Lhs, const Complete::Candidate &Rhs )
                      {
                          if ( Lhs.Kind != Rhs.Kind )
                          {
                              return static_cast<std::uint8_t>( Lhs.Kind ) < static_cast<std::uint8_t>( Rhs.Kind );
                          }
                          return Lhs.Text < Rhs.Text;
                      } );

    Candidates.erase( std::unique( Candidates.begin(), Candidates.end(),
                                   [] ( const Complete::Candidate &Lhs, const Complete::Candidate &Rhs )
                                   { return Lhs.Text == Rhs.Text and Lhs.Kind == Rhs.Kind; } ),
                      Candidates.end() );
}

// Members and free functions the REPL minted for itself are not names anybody
// typed, and offering them back would be offering bookkeeping.
[[nodiscard]] bool IsInternal ( const std::string_view Name )
{
    return Name.starts_with( "__" );
}

} // namespace

Volt::Repl::Complete::Completion Volt::Repl::Complete::Completer::At ( const std::string_view Line, const std::size_t Cursor )
{
    Completion Out;

    const std::size_t Where     = std::min( Cursor, Line.size() );
    const std::string_view Head = Line.substr( 0, Where );

    // --- The word being typed -------------------------------------------------
    std::size_t Begin = Where;
    while ( Begin > 0 and IsWordTail( Head[Begin - 1] ) )
    {
        --Begin;
    }

    Out.Begin = Begin;
    Out.End   = Where;

    const std::string_view Prefix = Head.substr( Begin, Where - Begin );
    const auto Matches = [&] ( const std::string_view Name ) { return Name.starts_with( Prefix ) and not IsInternal( Name ); };

    // --- `:builtin`, but only as the whole line -------------------------------
    //
    // A colon anywhere else is a symbol literal or a type annotation, and
    // offering `:layout` in the middle of `x : Int` would be nonsense.
    {
        const std::size_t Colon = Head.find_first_not_of( " \t" );
        if ( Colon != std::string_view::npos and Head[Colon] == ':' and Begin == Colon + 1 )
        {
            for ( const Query::Builtin &Each : Query::Builtins() )
            {
                if ( Each.Name.starts_with( Prefix ) )
                {
                    Out.Candidates.push_back( Candidate{
                        .Text = std::string( Each.Name ), .Detail = std::string( Each.Summary ), .Kind = EKind::Builtin } );
                }
            }
            Out.CommonPrefix = LongestCommonPrefix( Out.Candidates );
            return Out;
        }
    }

    // --- `expr.member` --------------------------------------------------------
    if ( Begin > 0 and Head[Begin - 1] == '.' )
    {
        const std::string_view Receiver = ReceiverBefore( Head, Begin - 1 );
        if ( Receiver.empty() )
        {
            return Out;
        }

        // A capitalised receiver may be a *type* rather than a value, and a
        // type cannot be typed as an expression. Asking the store directly is
        // what makes `String.` work.
        const std::vector<Evaluator::MemberFact> Members =
            Session.KnowsType( Receiver ) ? Session.MembersOfType( Receiver ) : Session.MembersOf( Receiver );

        for ( const Evaluator::MemberFact &Member : Members )
        {
            if ( not Matches( Member.Name ) )
            {
                continue;
            }

            std::string Detail = Member.bMethod ? Member.Signature + " -> " + Member.Result : Member.Result;
            if ( not Member.Owner.empty() )
            {
                Detail += "   (" + Member.Owner + ")";
            }

            Out.Candidates.push_back( Candidate{
                .Text = Member.Name, .Detail = std::move( Detail ), .Kind = Member.bMethod ? EKind::Member : EKind::Field } );
        }

        SortAndDedup( Out.Candidates );
        Out.CommonPrefix = LongestCommonPrefix( Out.Candidates );
        return Out;
    }

    // --- A bare name ----------------------------------------------------------
    //
    // Nearest first: what this session declared, then what the program can
    // call, then what it can name.
    for ( const Evaluator::VariableFact &Var : Session.Variables() )
    {
        if ( Matches( Var.Name ) )
        {
            Out.Candidates.push_back( Candidate{ .Text = Var.Name, .Detail = Var.Type, .Kind = EKind::Variable } );
        }
    }
    for ( const std::string &Name : Session.FunctionNames() )
    {
        if ( Matches( Name ) )
        {
            Out.Candidates.push_back( Candidate{ .Text = Name, .Detail = {}, .Kind = EKind::Function } );
        }
    }
    for ( const std::string &Name : Session.TypeNames() )
    {
        if ( Matches( Name ) )
        {
            Out.Candidates.push_back( Candidate{ .Text = Name, .Detail = {}, .Kind = EKind::Type } );
        }
    }

    SortAndDedup( Out.Candidates );
    Out.CommonPrefix = LongestCommonPrefix( Out.Candidates );
    return Out;
}

Volt::Repl::Doc::EPaletteRole Volt::Repl::Complete::Completer::Classify ( const std::string_view Name,
                                                                          const Doc::EPaletteRole Lexical ) const
{
    // Upgrades only, never downgrades. A `Constant` the store has not heard of
    // is a type the user is in the middle of typing, and painting it as an
    // error while they type it is exactly the kind of flicker a live
    // highlighter must not have.
    if ( Session.KnowsType( Name ) )
    {
        return Doc::EPaletteRole::TypeName;
    }
    if ( Lexical == Doc::EPaletteRole::Identifier and Session.KnowsFunction( Name ) )
    {
        return Doc::EPaletteRole::FunctionName;
    }
    return Lexical;
}

std::string Volt::Repl::Complete::Completer::GhostText ( const std::string_view Line, const std::span<const std::string> History )
{
    if ( Line.empty() )
    {
        return {};
    }

    // Newest first: what was typed most recently is what is most likely meant
    // again, which is the rule every shell that does this uses.
    for ( std::size_t Index = History.size(); Index > 0; --Index )
    {
        const std::string &Past = History[Index - 1];
        if ( Past.size() <= Line.size() or not Past.starts_with( Line ) )
        {
            continue;
        }

        // One line, never more. History remembers a `def ... end` as one entry
        // with real newlines in it, and a suggestion is drawn *inside* the
        // line being typed — so handing back the rest of a multi-line
        // statement would write newlines into the middle of a prompt and take
        // the cursor with them. The remainder of the first line is the only
        // part of it that can be shown, and an empty remainder is no
        // suggestion at all.
        const std::string_view Rest = std::string_view( Past ).substr( Line.size() );
        return std::string( Rest.substr( 0, Rest.find( '\n' ) ) );
    }
    return {};
}

Volt::Repl::Doc::Document Volt::Repl::Complete::Completer::Render ( const Completion &What,
                                                                    const Doc::Palette &Theme,
                                                                    const std::size_t Selected,
                                                                    const std::size_t MaxRows )
{
    Doc::Document Out;
    if ( What.Candidates.empty() or MaxRows == 0 )
    {
        return Out;
    }

    // A window around the selection rather than the head of the list: a
    // selection scrolled past the bottom would otherwise be invisible.
    const std::size_t Shown = std::min( MaxRows, What.Candidates.size() );
    std::size_t First       = 0;
    if ( Selected >= Shown )
    {
        First = std::min( Selected - Shown + 1, What.Candidates.size() - Shown );
    }

    std::size_t Widest = 0;
    for ( std::size_t Index = First; Index < First + Shown; ++Index )
    {
        Widest = std::max( Widest, Doc::DisplayWidth( What.Candidates[Index].Text ) );
    }

    for ( std::size_t Index = First; Index < First + Shown; ++Index )
    {
        const Candidate &Each        = What.Candidates[Index];
        const bool bSelected         = Index == Selected;
        const Doc::EPaletteRole Role = [&] ()
        {
            switch ( Each.Kind )
            {
            case EKind::Variable:
                return Doc::EPaletteRole::Identifier;
            case EKind::Function:
            case EKind::Member:
                return Doc::EPaletteRole::FunctionName;
            case EKind::Type:
                return Doc::EPaletteRole::TypeName;
            case EKind::Field:
                return Doc::EPaletteRole::Identifier;
            case EKind::Builtin:
                return Doc::EPaletteRole::Keyword;
            }
            return Doc::EPaletteRole::Default;
        }();

        Doc::Line Row;
        Row.Add( " " + Each.Text + std::string( Widest - Doc::DisplayWidth( Each.Text ), ' ' ) + " ",
                 bSelected ? Doc::RoleColor( Theme, Doc::EPaletteRole::Selection ) : Doc::RoleColor( Theme, Role ) );
        if ( not Each.Detail.empty() )
        {
            Row.Add( " " + Each.Detail, Doc::RoleColor( Theme, Doc::EPaletteRole::ResultType ) );
        }
        Out.Push( std::move( Row ) );
    }

    if ( What.Candidates.size() > Shown )
    {
        Out.PushText( " ... " + std::to_string( What.Candidates.size() - Shown ) + " more",
                      Doc::RoleColor( Theme, Doc::EPaletteRole::ResultType ) );
    }
    return Out;
}
