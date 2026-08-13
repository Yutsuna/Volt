// MacroExpansion.cpp — order 15. Resolves and expands `macro def` templates at
// compile time, before the other lowerings so generated code can use them:
//
// 1. Collects every MacroDef of the unit into a per-file name → DeclId map.
// 2. Replaces each MacroInvoke in a decl body with the declarations produced
//    by its expansion: the raw body is scanned into text fragments and
//    {% for %} / {% if %} / {{ }} template tags, the tags are evaluated over
//    compile-time MacroValues, and the emitted Volt text is re-lexed and
//    re-parsed with the ordinary Lexer/Parser into the same AstContext.
// 3. Iterates to a fixpoint (expansions may invoke further macros), bounded
//    by a depth cap.
//
// MacroValue is a compiler-internal value model (no Volt type names); member
// operations on values come from the MacroOps.inl manifest.

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"
#include "Volt/MiddleEnd/ConstEval/MagicConstants.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using namespace Volt;
using namespace Volt::Frontend;
using namespace Volt::MiddleEnd;
using namespace Volt::MiddleEnd::Core;
using namespace Volt::MiddleEnd::ConstEval;

// --- Compile-time values ----------------------------------------

struct MacroValue
{

    std::variant<std::monostate, bool, std::int64_t, std::string, std::vector<MacroValue>> Data;
};

[[nodiscard]] bool Truthy ( const MacroValue &Value )
{
    return std::visit( Meta::Overloaded{ [] ( std::monostate ) { return false; }, [] ( bool B ) { return B; },
                                         [] ( std::int64_t N ) { return N != 0; }, [] ( const auto & ) { return true; } },
                       Value.Data );
}

[[nodiscard]] std::string Stringify ( const MacroValue &Value )
{
    return std::visit( Meta::Overloaded{ [] ( std::monostate ) { return std::string{}; },
                                         [] ( bool B ) { return std::string( B ? "true" : "false" ); }, [] ( std::int64_t N )
                                         { return std::to_string( N ); }, [] ( const std::string &S ) { return S; },
                                         [] ( const std::vector<MacroValue> &Seq )
                                         {
                                             std::string Out = "[";
                                             for ( std::size_t Index = 0; Index < Seq.size(); ++Index )
                                             {
                                                 Out += Index > 0 ? ", " : "";
                                                 Out += Stringify( Seq[Index] );
                                             }
                                             return Out + "]";
                                         } },
                       Value.Data );
}

// --- Member operations, straight from the MacroOps.inl manifest --

using MacroOpFn = MacroValue ( * )( const MacroValue &Receiver,
                                    ::Volt::Core::DiagEngine::Bag &Diags,
                                    ::Volt::Core::SourceRange Loc );

#define VOLT_MACRO_OP( Name, Spelling )                                                                                          \
    [[nodiscard]] MacroValue Op##Name( const MacroValue &Receiver, ::Volt::Core::DiagEngine::Bag &Diags,                         \
                                       ::Volt::Core::SourceRange Loc );
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

MacroValue OpSize ( const MacroValue &Receiver, ::Volt::Core::DiagEngine::Bag &Diags, ::Volt::Core::SourceRange Loc )
{
    return std::visit( Meta::Overloaded{ [] ( const std::vector<MacroValue> &Seq )
                                         { return MacroValue{ static_cast<std::int64_t>( Seq.size() ) }; },
                                         [] ( const std::string &S )
                                         { return MacroValue{ static_cast<std::int64_t>( S.size() ) }; },
                                         [&] ( const auto & )
                                         {
                                             Diags.Error( Loc, "'size' expects a sequence or text macro value" );
                                             return MacroValue{};
                                         } },
                       Receiver.Data );
}

// --- Template scanning -------------------------------------------
// The body is raw text; tags are found by character scan so splices
// inside string literals ("\"{{ field }}\": ...") work naturally.

struct MacroSegment;
using SegmentList = std::vector<MacroSegment>;

struct TextSegment
{

    std::string_view Text;
};

struct SpliceSegment
{

    std::string_view ExprText;
};

struct ForSegment
{

    std::vector<std::string_view> Vars;
    std::string_view SeqText;
    SegmentList Body;
};

struct IfSegment
{

    std::string_view CondText;
    SegmentList Then;
    SegmentList Else;
};

struct MacroSegment
{

    std::variant<TextSegment, SpliceSegment, ForSegment, IfSegment> Data;
};

[[nodiscard]] std::string_view Trim ( std::string_view Text )
{
    while ( !Text.empty() and ( Text.front() == ' ' or Text.front() == '\t' or Text.front() == '\n' or Text.front() == '\r' ) )
    {
        Text.remove_prefix( 1 );
    }
    while ( !Text.empty() and ( Text.back() == ' ' or Text.back() == '\t' or Text.back() == '\n' or Text.back() == '\r' ) )
    {
        Text.remove_suffix( 1 );
    }
    return Text;
}

class TemplateScanner
{

public:

    enum class EStop : std::uint8_t
    {
        Eof,
        End,
        Else,
    };

    TemplateScanner ( std::string_view InSource, ::Volt::Core::DiagEngine::Bag &InDiags, ::Volt::Core::SourceRange InLoc )
        : Source( InSource ), Diags( InDiags ), Loc( InLoc )
    {
    }

    /// Scan segments until a closing `{% end %}` / `{% else %}` (or
    /// the end of the body, for the top level).
    EStop ScanBlock ( SegmentList &Out )
    {
        while ( Pos < Source.size() )
        {
            const std::size_t TagStart = Source.find( '{', Pos );
            const bool bSplice =
                TagStart != std::string_view::npos and TagStart + 1 < Source.size() and Source[TagStart + 1] == '{';
            const bool bDirective =
                TagStart != std::string_view::npos and TagStart + 1 < Source.size() and Source[TagStart + 1] == '%';

            const std::size_t TextEnd = TagStart == std::string_view::npos ? Source.size() : TagStart;
            if ( TextEnd > Pos )
            {
                Out.push_back( MacroSegment{ TextSegment{ Source.substr( Pos, TextEnd - Pos ) } } );
            }
            if ( TagStart == std::string_view::npos )
            {
                break;
            }
            if ( !bSplice and !bDirective )
            {
                // A lone `{` is ordinary text (hash literals, ...).
                Out.push_back( MacroSegment{ TextSegment{ Source.substr( TagStart, 1 ) } } );
                Pos = TagStart + 1;
                continue;
            }

            const std::string_view Close = bSplice ? "}}" : "%}";
            const std::size_t TagEnd     = Source.find( Close, TagStart + 2 );
            if ( TagEnd == std::string_view::npos )
            {
                Diags.Error( Loc, std::string( "unterminated macro tag (expected '" ) + std::string( Close ) + "')" );
                Pos = Source.size();
                break;
            }

            const std::string_view Inner = Trim( Source.substr( TagStart + 2, TagEnd - TagStart - 2 ) );
            Pos                          = TagEnd + 2;

            if ( bSplice )
            {
                Out.push_back( MacroSegment{ SpliceSegment{ Inner } } );
                continue;
            }

            if ( Inner == "end" )
            {
                return EStop::End;
            }
            if ( Inner == "else" )
            {
                return EStop::Else;
            }
            if ( Inner.starts_with( "for " ) )
            {
                ScanFor( Inner.substr( 4 ), Out );
                continue;
            }
            if ( Inner.starts_with( "if " ) )
            {
                ScanIf( Inner.substr( 3 ), Out );
                continue;
            }
            Diags.Error( Loc, "unknown macro directive '{% " + std::string( Inner ) + " %}'" );
        }
        return EStop::Eof;
    }

private:

    void ScanFor ( std::string_view Header, SegmentList &Out )
    {
        ForSegment Node;
        const std::size_t In = Header.find( " in " );
        if ( In == std::string_view::npos )
        {
            Diags.Error( Loc, "malformed '{% for %}' directive (expected 'for var[, index] in expr')" );
            return;
        }

        std::string_view Vars = Header.substr( 0, In );
        while ( !Vars.empty() )
        {
            const std::size_t Comma = Vars.find( ',' );
            Node.Vars.push_back( Trim( Vars.substr( 0, Comma ) ) );
            Vars = Comma == std::string_view::npos ? std::string_view{} : Vars.substr( Comma + 1 );
        }
        Node.SeqText = Trim( Header.substr( In + 4 ) );

        if ( ScanBlock( Node.Body ) != EStop::End )
        {
            Diags.Error( Loc, "unterminated '{% for %}' (expected '{% end %}')" );
        }
        Out.push_back( MacroSegment{ std::move( Node ) } );
    }

    void ScanIf ( std::string_view Header, SegmentList &Out )
    {
        IfSegment Node;
        Node.CondText = Trim( Header );

        EStop Stop = ScanBlock( Node.Then );
        if ( Stop == EStop::Else )
        {
            Stop = ScanBlock( Node.Else );
        }
        if ( Stop != EStop::End )
        {
            Diags.Error( Loc, "unterminated '{% if %}' (expected '{% end %}')" );
        }
        Out.push_back( MacroSegment{ std::move( Node ) } );
    }

    std::string_view Source;
    std::size_t Pos = 0;
    ::Volt::Core::DiagEngine::Bag &Diags;
    ::Volt::Core::SourceRange Loc;
};

// --- The expander ------------------------------------------------

class MacroExpander
{

public:

    explicit MacroExpander ( Volt::MiddleEnd::Core::PassContext &InContext ) : Context( InContext ), Ast( InContext.Ast )
    {
    }

    std::size_t Run ()
    {
        constexpr int MaxDepth = 32;
        std::size_t Total      = 0;

        for ( int Round = 0; Round < MaxDepth; ++Round )
        {
            CollectDefs();
            const std::size_t Expanded = ExpandRound();
            Total += Expanded;
            if ( Expanded == 0 )
            {
                DropDefsFromRoot();
                return Total;
            }
        }

        Context.Diags.Error( {}, "macro expansion exceeded the depth limit" );
        return Total;
    }

private:

    // A `macro` is a compile-time template: once every invocation is expanded
    // it carries no code a backend could emit, yet it stayed in TopDecls and
    // walked out of the middle-end with everything else. Only the root list is
    // cleaned — the node stays in the arena, so nothing that already holds its
    // DeclId (the Defs map above, a diagnostic's SourceRange) is invalidated.
    void DropDefsFromRoot ()
    {
        std::erase_if( Ast.TopDecls, [this] ( const Frontend::DeclId Id )
                       { return Id.IsValid() and KindOf( Ast.Decl( Id ) ) == Frontend::DeclKind::MacroDef; } );
    }

    void CollectDefs ()
    {
        Defs.clear();
        const std::size_t Count = Ast.DeclCount();
        for ( std::size_t Index = 0; Index < Count; ++Index )
        {
            const Frontend::DeclId Id{ static_cast<Frontend::DeclId::ValueType>( Index ) };
            if ( KindOf( Ast.Decl( Id ) ) == Frontend::DeclKind::MacroDef )
            {
                Defs.insert_or_assign( Ast.Text( std::get<Frontend::MacroDef>( Ast.Decl( Id ) ).Name ), Id );
            }
        }
    }

    std::size_t ExpandRound ()
    {
        // Snapshot: expansions append decls, which the next round
        // revisits (macros invoking macros reach the fixpoint there).
        const std::size_t Count = Ast.DeclCount();
        std::size_t Expanded    = 0;

        for ( std::size_t Index = 0; Index < Count; ++Index )
        {
            const Frontend::DeclId Id{ static_cast<Frontend::DeclId::ValueType>( Index ) };
            switch ( KindOf( Ast.Decl( Id ) ) )
            {
            case Frontend::DeclKind::Module:
                Expanded += ExpandIn<Frontend::Module>( Id );
                break;
            case Frontend::DeclKind::Class:
                Expanded += ExpandIn<Frontend::Class>( Id );
                break;
            case Frontend::DeclKind::Struct:
                Expanded += ExpandIn<Frontend::Struct>( Id );
                break;
            case Frontend::DeclKind::Mixin:
                Expanded += ExpandIn<Frontend::Mixin>( Id );
                break;
            case Frontend::DeclKind::Enum:
                Expanded += ExpandIn<Frontend::Enum>( Id );
                break;
            default:
                break;
            }
        }
        return Expanded;
    }

    /// Replace every MacroInvoke of the node's body with its
    /// expansion. Copy-out / write-back: expansion appends to the
    /// decl arena, which may reallocate the slot.
    template <typename NodeType> [[nodiscard]] std::size_t ExpandIn ( Frontend::DeclId Id )
    {
        NodeType Node = std::get<NodeType>( Ast.Decl( Id ) );

        Frontend::DeclList NewBody;
        std::size_t Expanded = 0;
        for ( const Frontend::DeclId Child : Node.Body )
        {
            if ( !Child.IsValid() or KindOf( Ast.Decl( Child ) ) != Frontend::DeclKind::MacroInvoke )
            {
                NewBody.PushBack( Child );
                continue;
            }

            const Frontend::MacroInvoke Invoke = std::get<Frontend::MacroInvoke>( Ast.Decl( Child ) );
            for ( const Frontend::DeclId Generated : ExpandInvoke( Invoke ) )
            {
                NewBody.PushBack( Generated );
            }
            ++Expanded;
        }

        if ( Expanded > 0 )
        {
            Node.Body      = std::move( NewBody );
            Ast.Decl( Id ) = Frontend::DeclNode{ std::move( Node ) };
        }
        return Expanded;
    }

    [[nodiscard]] Frontend::DeclList ExpandInvoke ( const Frontend::MacroInvoke &Invoke )
    {
        const auto Found = Defs.find( Ast.Text( Invoke.Name ) );
        if ( Found == Defs.end() )
        {
            Context.Diags.Error( Invoke.Loc, "no macro named '" + std::string( Ast.Text( Invoke.Name ) ) + "' in this file" );
            return {};
        }
        const Frontend::MacroDef Def = std::get<Frontend::MacroDef>( Ast.Decl( Found->second ) );

        CurrentLoc = Invoke.Loc;
        BindArguments( Invoke, Def );

        TemplateScanner Scanner( Ast.Text( Def.BodyText ), Context.Diags, Invoke.Loc );
        SegmentList Segments;
        if ( Scanner.ScanBlock( Segments ) != TemplateScanner::EStop::Eof )
        {
            Context.Diags.Error( Invoke.Loc, "stray '{% end %}' or '{% else %}' in macro body" );
        }

        std::string Generated;
        Emit( Segments, Generated );

        // Re-parse the generated text with the ordinary front end,
        // into the same AstContext. Any error is anchored back to the
        // invocation site.
        const std::size_t ErrorsBefore = Context.Diags.Errors();
        const Frontend::ArenaMark Mark = Frontend::MarkArenas( Ast );
        Frontend::Lexer GeneratedLexer( Ast.FileId(), Generated, Ast.Strings(), Context.Diags );
        Frontend::Parser GeneratedParser( GeneratedLexer.Tokenize(), Ast, Context.Diags, Generated );
        Frontend::DeclList Out;
        GeneratedParser.ParseMemberBlock( Out );

        // The generated text is synthesised, not a slice of the file,
        // so no node in it has a real preimage. The invocation is the
        // one honest location for all of it — which is also what
        // makes `__LINE__` in a macro body report the caller's line.
        StampLocsSince( Ast, Mark, Invoke.Loc );

        if ( Context.Diags.Errors() > ErrorsBefore )
        {
            Context.Diags.Error( Invoke.Loc, "in expansion of macro '" + std::string( Ast.Text( Invoke.Name ) ) + "'" );
        }
        return Out;
    }

    void BindArguments ( const MacroInvoke &Invoke, const MacroDef &Def )
    {
        Env.clear();

        std::vector<std::string_view> ParamNames;
        for ( const ParamId Id : Def.Params )
        {
            ParamNames.push_back( Ast.Text( Ast.GetParam( Id ).Name ) );
        }

        std::size_t Positional = 0;
        for ( std::size_t Index = 0; Index < Invoke.Args.Size(); ++Index )
        {
            const Symbol Name = Index < Invoke.ArgNames.Size() ? Invoke.ArgNames[Index] : Symbol{};
            MacroValue Value  = Eval( Invoke.Args[Index] );

            if ( Name.IsValid() )
            {
                const std::string_view Text = Ast.Text( Name );
                if ( std::ranges::find( ParamNames, Text ) == ParamNames.end() )
                {
                    Context.Diags.Error( Invoke.Loc, "macro '" + std::string( Ast.Text( Def.Name ) ) + "' has no parameter '" +
                                                         std::string( Text ) + "'" );
                    continue;
                }
                Env.insert_or_assign( std::string( Text ), std::move( Value ) );
            }
            else if ( Positional < ParamNames.size() )
            {
                Env.insert_or_assign( std::string( ParamNames[Positional++] ), std::move( Value ) );
            }
            else
            {
                Context.Diags.Error( Invoke.Loc, "too many arguments for macro '" + std::string( Ast.Text( Def.Name ) ) + "'" );
            }
        }
    }

    // --- Tag evaluation ------------------------------------------

    [[nodiscard]] MacroValue EvalText ( std::string_view Text, ::Volt::Core::SourceRange Loc )
    {
        Lexer TagLexer( Ast.FileId(), Text, Ast.Strings(), Context.Diags );
        Parser TagParser( TagLexer.Tokenize(), Ast, Context.Diags, Text );
        const ExprId Id = TagParser.ParseExpression();
        if ( !Id.IsValid() )
        {
            Context.Diags.Error( Loc, "invalid macro tag expression '" + std::string( Text ) + "'" );
            return {};
        }
        return Eval( Id );
    }

    [[nodiscard]] MacroValue Eval ( ExprId Id )
    {
        const ::Volt::Core::SourceRange Loc = CurrentLoc;
        return std::visit(
            Meta::Overloaded{
                [&] ( const Frontend::IntLiteral &Node )
                {
                    std::int64_t Value          = 0;
                    const std::string_view Text = Ast.Text( Node.Raw );
                    std::from_chars( Text.data(), Text.data() + Text.size(), Value );
                    return MacroValue{ Value };
                },
                [&] ( const Frontend::StringLiteral &Node ) { return MacroValue{ std::string( Ast.Text( Node.Value ) ) }; },
                [&] ( const Frontend::SymbolLiteral &Node )
                {
                    // The interned lexeme keeps the leading ':'; a symbol
                    // splices as the bare name.
                    std::string_view Text = Ast.Text( Node.Name );
                    if ( Text.starts_with( ':' ) )
                    {
                        Text.remove_prefix( 1 );
                    }
                    return MacroValue{ std::string( Text ) };
                },
                [&] ( const Frontend::BoolLiteral &Node ) { return MacroValue{ Node.Value }; },
                [&] ( const Frontend::ArrayLit &Node )
                {
                    std::vector<MacroValue> Elements;
                    for ( const ExprId Element : Node.Elements )
                    {
                        Elements.push_back( Eval( Element ) );
                    }
                    return MacroValue{ std::move( Elements ) };
                },
                [&] ( const Frontend::Identifier &Node )
                {
                    const auto Found = Env.find( std::string( Ast.Text( Node.Name ) ) );
                    if ( Found == Env.end() )
                    {
                        Context.Diags.Error( Loc,
                                             "unknown name '" + std::string( Ast.Text( Node.Name ) ) + "' in macro expression" );
                        return MacroValue{};
                    }
                    return Found->second;
                },
                [&] ( const Frontend::Member &Node ) { return ApplyOp( Eval( Node.Object ), Ast.Text( Node.Name ), Loc ); },
                [&] ( const Frontend::Call &Node )
                {
                    // `fields.size()` — zero-arg member call sugar.
                    if ( Node.Args.Size() == 0 and Node.Callee.IsValid() and
                         KindOf( Ast.Expr( Node.Callee ) ) == ExprKind::Member )
                    {
                        return Eval( Node.Callee );
                    }
                    Context.Diags.Error( Loc, "unsupported call in macro expression" );
                    return MacroValue{};
                },
                [&] ( const Frontend::Binary &Node ) { return EvalBinary( Node, Loc ); },
                [&] ( const Frontend::Unary &Node )
                {
                    const MacroValue Operand = Eval( Node.Operand );
                    if ( Node.Op == TokenKind::Minus and std::holds_alternative<std::int64_t>( Operand.Data ) )
                    {
                        return MacroValue{ -std::get<std::int64_t>( Operand.Data ) };
                    }
                    if ( Node.Op == TokenKind::Bang or Node.Op == TokenKind::KwNot )
                    {
                        return MacroValue{ !Truthy( Operand ) };
                    }
                    Context.Diags.Error( Loc, "unsupported unary operator in macro expression" );
                    return MacroValue{};
                },
                [&] ( const auto & )
                {
                    Context.Diags.Error( Loc, "unsupported construct in macro expression" );
                    return MacroValue{};
                } },
            Ast.Expr( Id ) );
    }

    [[nodiscard]] MacroValue EvalBinary ( const Frontend::Binary &Node, ::Volt::Core::SourceRange Loc )
    {
        if ( Node.Op == TokenKind::AndAnd or Node.Op == TokenKind::KwAnd )
        {
            return MacroValue{ Truthy( Eval( Node.Lhs ) ) and Truthy( Eval( Node.Rhs ) ) };
        }
        if ( Node.Op == TokenKind::OrOr or Node.Op == TokenKind::KwOr )
        {
            return MacroValue{ Truthy( Eval( Node.Lhs ) ) or Truthy( Eval( Node.Rhs ) ) };
        }

        const MacroValue Lhs = Eval( Node.Lhs );
        const MacroValue Rhs = Eval( Node.Rhs );

        if ( std::holds_alternative<std::int64_t>( Lhs.Data ) and std::holds_alternative<std::int64_t>( Rhs.Data ) )
        {
            const std::int64_t L = std::get<std::int64_t>( Lhs.Data );
            const std::int64_t R = std::get<std::int64_t>( Rhs.Data );
            switch ( Node.Op )
            {
            case TokenKind::Plus:
                return MacroValue{ L + R };
            case TokenKind::Minus:
                return MacroValue{ L - R };
            case TokenKind::Star:
                return MacroValue{ L * R };
            case TokenKind::Slash:
                return MacroValue{ R != 0 ? L / R : 0 };
            case TokenKind::Percent:
                return MacroValue{ R != 0 ? L % R : 0 };
            case TokenKind::Lt:
                return MacroValue{ L < R };
            case TokenKind::Le:
                return MacroValue{ L <= R };
            case TokenKind::Gt:
                return MacroValue{ L > R };
            case TokenKind::Ge:
                return MacroValue{ L >= R };
            case TokenKind::EqEq:
                return MacroValue{ L == R };
            case TokenKind::NotEq:
                return MacroValue{ L != R };
            default:
                break;
            }
        }
        else if ( std::holds_alternative<std::string>( Lhs.Data ) and std::holds_alternative<std::string>( Rhs.Data ) )
        {
            const std::string &L = std::get<std::string>( Lhs.Data );
            const std::string &R = std::get<std::string>( Rhs.Data );
            switch ( Node.Op )
            {
            case TokenKind::Plus:
                return MacroValue{ L + R };
            case TokenKind::EqEq:
                return MacroValue{ L == R };
            case TokenKind::NotEq:
                return MacroValue{ L != R };
            default:
                break;
            }
        }

        Context.Diags.Error( Loc, "unsupported operand types in macro expression" );
        return {};
    }

    [[nodiscard]] MacroValue ApplyOp ( const MacroValue &Receiver, std::string_view Spelling, ::Volt::Core::SourceRange Loc )
    {
        for ( const MacroOpRow &Op : MacroOps )
        {
            if ( Op.Spelling == Spelling )
            {
                return Op.Apply( Receiver, Context.Diags, Loc );
            }
        }
        Context.Diags.Error( Loc, "unknown macro operation '" + std::string( Spelling ) + "'" );
        return {};
    }

    // --- Emission ------------------------------------------------

    void Emit ( const SegmentList &Segments, std::string &Out )
    {
        for ( const MacroSegment &Segment : Segments )
        {
            std::visit(
                Meta::Overloaded{ [&] ( const TextSegment &Node ) { Out += Node.Text; }, [&] ( const SpliceSegment &Node )
                                  { Out += Stringify( EvalText( Node.ExprText, CurrentLoc ) ); }, [&] ( const IfSegment &Node )
                                  { Emit( Truthy( EvalText( Node.CondText, CurrentLoc ) ) ? Node.Then : Node.Else, Out ); },
                                  [&] ( const ForSegment &Node ) { EmitFor( Node, Out ); } },
                Segment.Data );
        }
    }

    void EmitFor ( const ForSegment &Node, std::string &Out )
    {
        const MacroValue Seq = EvalText( Node.SeqText, CurrentLoc );
        if ( !std::holds_alternative<std::vector<MacroValue>>( Seq.Data ) )
        {
            Context.Diags.Error( CurrentLoc, "'{% for %}' expects a sequence macro value" );
            return;
        }

        const std::vector<MacroValue> &Elements = std::get<std::vector<MacroValue>>( Seq.Data );
        for ( std::size_t Index = 0; Index < Elements.size(); ++Index )
        {
            if ( !Node.Vars.empty() )
            {
                Env.insert_or_assign( std::string( Node.Vars[0] ), Elements[Index] );
            }
            if ( Node.Vars.size() > 1 )
            {
                Env.insert_or_assign( std::string( Node.Vars[1] ), MacroValue{ static_cast<std::int64_t>( Index ) } );
            }
            Emit( Node.Body, Out );
        }
    }

    PassContext &Context;
    Frontend::AstContext &Ast;
    std::unordered_map<std::string_view, Frontend::DeclId> Defs;
    std::unordered_map<std::string, MacroValue> Env;
    ::Volt::Core::SourceRange CurrentLoc;
};

} // namespace

namespace Volt::MiddleEnd::ConstEval
{

void MacroExpansion ( PassContext &Context )
{
    Context.Stats.MacrosExpanded += MacroExpander( Context ).Run();
}

} // namespace Volt::MiddleEnd::ConstEval
