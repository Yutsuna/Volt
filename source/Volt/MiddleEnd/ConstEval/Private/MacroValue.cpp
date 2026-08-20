#include "MacroValue.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Support/ProcessRunner.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <utility>

namespace
{

using namespace Volt;
using namespace Volt::MiddleEnd::ConstEval;

// A command that outlives these is a build that hangs or a build that eats its
// own memory; both are worse than a build that stops with a diagnostic.
constexpr ::Volt::Core::ProcessLimits CommandLimits{ .TimeoutMs = 10'000, .MaxBytes = 1U << 20 };

// A command's stderr is the only thing that explains why it failed, and its
// first line is nearly always the whole explanation.
[[nodiscard]] std::string FirstLineOf ( const std::string &Text )
{
    if ( Text.empty() )
    {
        return {};
    }
    const std::size_t Break = Text.find( '\n' );
    return ": " + Text.substr( 0, Break == std::string::npos ? Text.size() : Break );
}

// --- Member operations, straight from the MacroOps.inl manifest ------------

using MacroOpFn = std::optional<MacroValue> ( * )( const MacroValue &Receiver,
                                                   std::span<const MacroValue> Args,
                                                   ::Volt::Core::DiagEngine::Bag &Diags,
                                                   ::Volt::Core::SourceRange Loc );

#define VOLT_MACRO_OP( Name, Spelling )                                                                                          \
    [[nodiscard]] std::optional<MacroValue> Op##Name( const MacroValue &Receiver, std::span<const MacroValue> Args,              \
                                                      ::Volt::Core::DiagEngine::Bag &Diags, ::Volt::Core::SourceRange Loc );
#include "Volt/MiddleEnd/ConstEval/MacroOps.inl"

struct MacroOpRow
{

    std::string_view Spelling;
    MacroOpFn Apply = nullptr;
};

constexpr std::array MacroOps = {
#define VOLT_MACRO_OP( Name, Spelling ) MacroOpRow{ Spelling, &Op##Name },
#include "Volt/MiddleEnd/ConstEval/MacroOps.inl"
};

[[nodiscard]] const std::string *AsText ( const MacroValue &Value )
{
    return std::get_if<std::string>( &Value.Data );
}

/// The one shape every text operation needs: a text receiver, and at most one
/// text argument. Written once so a new row is a body, not a preamble.
[[nodiscard]] bool TextOperands ( std::string_view Op,
                                  const MacroValue &Receiver,
                                  std::span<const MacroValue> Args,
                                  std::size_t MaxArgs,
                                  ::Volt::Core::DiagEngine::Bag &Diags,
                                  ::Volt::Core::SourceRange Loc,
                                  const std::string *&Text,
                                  const std::string *&Argument )
{
    Text     = AsText( Receiver );
    Argument = nullptr;

    if ( Text == nullptr )
    {
        Diags.Error( Loc, "'" + std::string( Op ) + "' expects text at compile time" );
        return false;
    }
    if ( Args.size() > MaxArgs )
    {
        Diags.Error( Loc, "'" + std::string( Op ) + "' takes at most " + std::to_string( MaxArgs ) + " argument(s)" );
        return false;
    }
    if ( not Args.empty() )
    {
        Argument = AsText( Args[0] );
        if ( Argument == nullptr )
        {
            Diags.Error( Loc, "'" + std::string( Op ) + "' expects a text argument at compile time" );
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view TrimText ( std::string_view Text )
{
    constexpr std::string_view Space = " \t\r\n";
    const std::size_t First          = Text.find_first_not_of( Space );
    if ( First == std::string_view::npos )
    {
        return {};
    }
    return Text.substr( First, Text.find_last_not_of( Space ) - First + 1 );
}

std::optional<MacroValue> OpSize ( const MacroValue &Receiver,
                                   std::span<const MacroValue> Args,
                                   ::Volt::Core::DiagEngine::Bag &Diags,
                                   ::Volt::Core::SourceRange Loc )
{
    static_cast<void>( Args );
    return std::visit( Meta::Overloaded{ [] ( const std::vector<MacroValue> &Seq ) -> std::optional<MacroValue>
                                         { return MacroValue{ static_cast<std::int64_t>( Seq.size() ) }; },
                                         [] ( const std::string &Text ) -> std::optional<MacroValue>
                                         { return MacroValue{ static_cast<std::int64_t>( Text.size() ) }; },
                                         [&] ( const auto & ) -> std::optional<MacroValue>
                                         {
                                             Diags.Error( Loc, "'size' expects a sequence or text macro value" );
                                             return MacroValue{};
                                         } },
                       Receiver.Data );
}

// `` `find ...`.lines `` — the shape every command-driven loop starts from. A
// trailing newline yields no empty last line: a command's output ends in one,
// and an iteration over nothing is never what the source meant.
std::optional<MacroValue> OpLines ( const MacroValue &Receiver,
                                    std::span<const MacroValue> Args,
                                    ::Volt::Core::DiagEngine::Bag &Diags,
                                    ::Volt::Core::SourceRange Loc )
{
    const std::string *Text     = nullptr;
    const std::string *Argument = nullptr;
    if ( not TextOperands( "lines", Receiver, Args, 0, Diags, Loc, Text, Argument ) )
    {
        return {};
    }

    std::vector<MacroValue> Out;
    std::size_t Start = 0;
    while ( Start <= Text->size() )
    {
        const std::size_t Break = Text->find( '\n', Start );
        const std::size_t End   = Break == std::string::npos ? Text->size() : Break;
        std::string Line        = Text->substr( Start, End - Start );
        if ( not Line.empty() and Line.back() == '\r' )
        {
            Line.pop_back();
        }
        if ( Break == std::string::npos )
        {
            if ( not Line.empty() )
            {
                Out.push_back( MacroValue{ std::move( Line ) } );
            }
            break;
        }
        Out.push_back( MacroValue{ std::move( Line ) } );
        Start = Break + 1;
    }
    return MacroValue{ std::move( Out ) };
}

std::optional<MacroValue> OpTrim ( const MacroValue &Receiver,
                                   std::span<const MacroValue> Args,
                                   ::Volt::Core::DiagEngine::Bag &Diags,
                                   ::Volt::Core::SourceRange Loc )
{
    const std::string *Text     = nullptr;
    const std::string *Argument = nullptr;
    if ( not TextOperands( "trim", Receiver, Args, 0, Diags, Loc, Text, Argument ) )
    {
        return {};
    }
    return MacroValue{ std::string( TrimText( *Text ) ) };
}

// `chomp` with no argument drops one trailing newline; with one, that exact
// suffix — `json.chomp( "," )`.
std::optional<MacroValue> OpChomp ( const MacroValue &Receiver,
                                    std::span<const MacroValue> Args,
                                    ::Volt::Core::DiagEngine::Bag &Diags,
                                    ::Volt::Core::SourceRange Loc )
{
    const std::string *Text     = nullptr;
    const std::string *Argument = nullptr;
    if ( not TextOperands( "chomp", Receiver, Args, 1, Diags, Loc, Text, Argument ) )
    {
        return {};
    }

    std::string Out = *Text;
    if ( Argument == nullptr )
    {
        if ( not Out.empty() and Out.back() == '\n' )
        {
            Out.pop_back();
        }
        if ( not Out.empty() and Out.back() == '\r' )
        {
            Out.pop_back();
        }
        return MacroValue{ std::move( Out ) };
    }
    if ( not Argument->empty() and Out.size() >= Argument->size() and Out.ends_with( *Argument ) )
    {
        Out.resize( Out.size() - Argument->size() );
    }
    return MacroValue{ std::move( Out ) };
}

// `basename` of a path, optionally without a suffix: `file.basename( ".vl" )`.
std::optional<MacroValue> OpBasename ( const MacroValue &Receiver,
                                       std::span<const MacroValue> Args,
                                       ::Volt::Core::DiagEngine::Bag &Diags,
                                       ::Volt::Core::SourceRange Loc )
{
    const std::string *Text     = nullptr;
    const std::string *Argument = nullptr;
    if ( not TextOperands( "basename", Receiver, Args, 1, Diags, Loc, Text, Argument ) )
    {
        return {};
    }

    const std::size_t Slash = Text->find_last_of( '/' );
    std::string Out         = Slash == std::string::npos ? *Text : Text->substr( Slash + 1 );
    if ( Argument != nullptr and not Argument->empty() and Out.ends_with( *Argument ) )
    {
        Out.resize( Out.size() - Argument->size() );
    }
    return MacroValue{ std::move( Out ) };
}

// `self.fields` — the target type's own fields, in declaration order, captured
// when `self` was evaluated. A receiver that is not the target type declines:
// `fields` on a runtime value is that value's own member and the type checker
// resolves it there.
std::optional<MacroValue> OpFields ( const MacroValue &Receiver,
                                     std::span<const MacroValue> Args,
                                     ::Volt::Core::DiagEngine::Bag &Diags,
                                     ::Volt::Core::SourceRange Loc )
{
    const auto *Type = std::get_if<MacroTypeDesc>( &Receiver.Data );
    if ( Type == nullptr )
    {
        return std::nullopt;
    }
    if ( not Args.empty() )
    {
        Diags.Error( Loc, "'fields' takes no argument" );
        return std::nullopt;
    }

    std::vector<MacroValue> Out;
    Out.reserve( Type->Fields.size() );
    for ( const MacroFieldDesc &Field : Type->Fields )
    {
        Out.push_back( MacroValue{ Field } );
    }
    return MacroValue{ std::move( Out ) };
}

// `self.name` and `field.name` are the same question asked of the two things
// introspection hands out, so they are one row rather than two.
std::optional<MacroValue> OpName ( const MacroValue &Receiver,
                                   std::span<const MacroValue> Args,
                                   ::Volt::Core::DiagEngine::Bag &Diags,
                                   ::Volt::Core::SourceRange Loc )
{
    static_cast<void>( Args );
    static_cast<void>( Diags );
    static_cast<void>( Loc );
    return std::visit(
        Meta::Overloaded{ [] ( const MacroTypeDesc &Type ) -> std::optional<MacroValue> { return MacroValue{ Type.Name }; },
                          [] ( const MacroFieldDesc &Field ) -> std::optional<MacroValue> { return MacroValue{ Field.Name }; },
                          [] ( const auto & ) -> std::optional<MacroValue> { return std::nullopt; } },
        Receiver.Data );
}

// `field.type` — the spelling the source wrote, never a resolved type: this
// runs before signatures are resolved, and generation wants the user's own
// words anyway.
std::optional<MacroValue> OpType ( const MacroValue &Receiver,
                                   std::span<const MacroValue> Args,
                                   ::Volt::Core::DiagEngine::Bag &Diags,
                                   ::Volt::Core::SourceRange Loc )
{
    static_cast<void>( Args );
    static_cast<void>( Diags );
    static_cast<void>( Loc );
    const auto *Field = std::get_if<MacroFieldDesc>( &Receiver.Data );
    if ( Field == nullptr )
    {
        return std::nullopt;
    }
    return MacroValue{ Field->Type };
}

} // namespace

namespace Volt::MiddleEnd::ConstEval
{

bool Truthy ( const MacroValue &Value )
{
    return std::visit( Meta::Overloaded{ [] ( std::monostate ) { return false; }, [] ( bool B ) { return B; },
                                         [] ( std::int64_t N ) { return N != 0; }, [] ( const auto & ) { return true; } },
                       Value.Data );
}

std::string Stringify ( const MacroValue &Value )
{
    return std::visit( Meta::Overloaded{ [] ( std::monostate ) { return std::string{}; },
                                         [] ( bool B ) { return std::string( B ? "true" : "false" ); }, [] ( std::int64_t N )
                                         { return std::to_string( N ); }, [] ( const std::string &Text ) { return Text; },
                                         [] ( const std::vector<MacroValue> &Seq )
                                         {
                                             std::string Out = "[";
                                             for ( std::size_t Index = 0; Index < Seq.size(); ++Index )
                                             {
                                                 Out += Index > 0 ? ", " : "";
                                                 Out += Stringify( Seq[Index] );
                                             }
                                             return Out + "]";
                                         },
                                         // A field and a type each render as their name, which is what
                                         // `#{field}` and `@#{field}` are written to mean.
                                         [] ( const MacroFieldDesc &Field ) { return Field.Name; },
                                         [] ( const MacroTypeDesc &Type ) { return Type.Name; } },
                       Value.Data );
}

std::optional<MacroValue> ValueOfLiteral ( const Frontend::AstContext &Ast, Frontend::ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return std::nullopt;
    }
    return ValueOfLiteralNode( Ast, Ast.Expr( Id ) );
}

std::optional<MacroValue> ValueOfLiteralNode ( const Frontend::AstContext &Ast, const Frontend::ExprNode &Literal )
{
    return std::visit( Meta::Overloaded{ [&] ( const Frontend::StringLiteral &Node ) -> std::optional<MacroValue>
                                         { return MacroValue{ std::string( Ast.Text( Node.Value ) ) }; },
                                         [&] ( const Frontend::BoolLiteral &Node ) -> std::optional<MacroValue>
                                         { return MacroValue{ Node.Value }; },
                                         [&] ( const Frontend::IntLiteral &Node ) -> std::optional<MacroValue>
                                         {
                                             std::int64_t Value          = 0;
                                             const std::string_view Text = Ast.Text( Node.Raw );
                                             const auto [Ptr, Code] =
                                                 std::from_chars( Text.data(), Text.data() + Text.size(), Value );
                                             if ( Code != std::errc{} )
                                             {
                                                 return std::nullopt;
                                             }
                                             return MacroValue{ Value };
                                         },
                                         [&] ( const Frontend::SymbolLiteral &Node ) -> std::optional<MacroValue>
                                         {
                                             // The interned lexeme keeps the leading ':'; a symbol is worth
                                             // its bare name, which is what a generated identifier needs.
                                             std::string_view Text = Ast.Text( Node.Name );
                                             if ( Text.starts_with( ':' ) )
                                             {
                                                 Text.remove_prefix( 1 );
                                             }
                                             return MacroValue{ std::string( Text ) };
                                         },
                                         [&] ( const Frontend::ArrayLit &Node ) -> std::optional<MacroValue>
                                         {
                                             std::vector<MacroValue> Elements;
                                             Elements.reserve( Node.Elements.Size() );
                                             for ( const Frontend::ExprId Element : Node.Elements )
                                             {
                                                 std::optional<MacroValue> Value = ValueOfLiteral( Ast, Element );
                                                 if ( not Value )
                                                 {
                                                     return std::nullopt; // one runtime element makes the whole array runtime
                                                 }
                                                 Elements.push_back( std::move( *Value ) );
                                             }
                                             return MacroValue{ std::move( Elements ) };
                                         },
                                         [] ( const auto & ) -> std::optional<MacroValue> { return std::nullopt; } },
                       Literal );
}

Frontend::ExprNode LiteralOfValue ( Frontend::AstContext &Ast, const MacroValue &Value, ::Volt::Core::SourceRange Loc )
{
    return std::visit(
        Meta::Overloaded{ [&] ( bool B ) -> Frontend::ExprNode { return Frontend::BoolLiteral{ .Loc = Loc, .Value = B }; },
                          [&] ( std::int64_t N ) -> Frontend::ExprNode
                          { return Frontend::IntLiteral{ .Loc = Loc, .Raw = Ast.Strings().Intern( std::to_string( N ) ) }; },
                          [&] ( const std::string &Text ) -> Frontend::ExprNode
                          { return Frontend::StringLiteral{ .Loc = Loc, .Value = Ast.Strings().Intern( Text ) }; },
                          [&] ( const std::vector<MacroValue> &Seq ) -> Frontend::ExprNode
                          {
                              Frontend::ArrayLit Node;
                              Node.Loc = Loc;
                              for ( const MacroValue &Element : Seq )
                              {
                                  Node.Elements.PushBack( Ast.Add( LiteralOfValue( Ast, Element, Loc ) ) );
                              }
                              return Node;
                          },
                          [&] ( std::monostate ) -> Frontend::ExprNode { return Frontend::NilLiteral{ .Loc = Loc }; },
                          // Introspection values reach a literal only through their name —
                          // `#{field}` in a generated string, never a value of their own.
                          [&] ( const MacroFieldDesc &Field ) -> Frontend::ExprNode
                          { return Frontend::StringLiteral{ .Loc = Loc, .Value = Ast.Strings().Intern( Field.Name ) }; },
                          [&] ( const MacroTypeDesc &Type ) -> Frontend::ExprNode
                          { return Frontend::StringLiteral{ .Loc = Loc, .Value = Ast.Strings().Intern( Type.Name ) }; } },
        Value.Data );
}

bool IsMacroOp ( std::string_view Spelling )
{
    return std::ranges::any_of( MacroOps, [&] ( const MacroOpRow &Op ) { return Op.Spelling == Spelling; } );
}

MacroValue RunMacroCommand ( const std::string &Command,
                             std::string_view WorkDir,
                             ::Volt::Core::DiagEngine::Bag &Diags,
                             ::Volt::Core::SourceRange Loc )
{
    const ::Volt::Core::ProcessResult Result = ::Volt::Core::RunShell( Command, WorkDir, CommandLimits );

    if ( Result.bSpawnFailed )
    {
        Diags.Error( Loc, "could not run `" + Command + "`: " + Result.Err );
    }
    else if ( Result.bTimedOut )
    {
        Diags.Error( Loc, "`" + Command + "` did not finish within " + std::to_string( CommandLimits.TimeoutMs ) + "ms" );
    }
    else if ( Result.ExitCode != 0 )
    {
        Diags.Error( Loc, "`" + Command + "` failed with exit status " + std::to_string( Result.ExitCode ) +
                              FirstLineOf( Result.Err ) );
    }
    else if ( Result.bTruncated )
    {
        Diags.Error( Loc,
                     "`" + Command + "` produced more than " + std::to_string( CommandLimits.MaxBytes ) + " bytes of output" );
    }

    return MacroValue{ Result.Out };
}

std::optional<MacroValue> ApplyMacroOp ( std::string_view Spelling,
                                         const MacroValue &Receiver,
                                         std::span<const MacroValue> Args,
                                         ::Volt::Core::DiagEngine::Bag &Diags,
                                         ::Volt::Core::SourceRange Loc )
{
    for ( const MacroOpRow &Op : MacroOps )
    {
        if ( Op.Spelling == Spelling )
        {
            return Op.Apply( Receiver, Args, Diags, Loc );
        }
    }
    return std::nullopt;
}

} // namespace Volt::MiddleEnd::ConstEval
