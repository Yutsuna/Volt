#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"

#include <cstdint>
#include <string>

namespace
{

// Keywords that open a block closed by `end`, for the raw-capture scan
// of a macro body. `{% for %}` / `{% if %}` template directives close
// with `{% end %}`, so their keywords stay balanced too. Known limits
// of the token-level count (not used by the current samples): postfix
// modifiers (`x if cond`) and body-less `abstract def`.
[[nodiscard]] bool IsBlockOpener ( Volt::Frontend::TokenKind Kind )
{
    switch ( Kind )
    {
    case Volt::Frontend::TokenKind::KwDef:
    case Volt::Frontend::TokenKind::KwDo:
    case Volt::Frontend::TokenKind::KwIf:
    case Volt::Frontend::TokenKind::KwUnless:
    case Volt::Frontend::TokenKind::KwWhile:
    case Volt::Frontend::TokenKind::KwFor:
    case Volt::Frontend::TokenKind::KwCase:
    case Volt::Frontend::TokenKind::KwClass:
    case Volt::Frontend::TokenKind::KwStruct:
    case Volt::Frontend::TokenKind::KwModule:
    case Volt::Frontend::TokenKind::KwMixin:
    case Volt::Frontend::TokenKind::KwComponent:
    case Volt::Frontend::TokenKind::KwEnum:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool IsOperatorMethodStart ( Volt::Frontend::TokenKind Kind )
{
    switch ( Kind )
    {
    case Volt::Frontend::TokenKind::LBracket:
    case Volt::Frontend::TokenKind::Spaceship:
    case Volt::Frontend::TokenKind::Bang:
    case Volt::Frontend::TokenKind::Plus:
    case Volt::Frontend::TokenKind::Minus:
    case Volt::Frontend::TokenKind::Star:
    case Volt::Frontend::TokenKind::Slash:
    case Volt::Frontend::TokenKind::Percent:
    case Volt::Frontend::TokenKind::TripleEq:
    case Volt::Frontend::TokenKind::EqEq:
    case Volt::Frontend::TokenKind::NotEq:
    case Volt::Frontend::TokenKind::Lt:
    case Volt::Frontend::TokenKind::Gt:
    case Volt::Frontend::TokenKind::Le:
    case Volt::Frontend::TokenKind::Ge:
    case Volt::Frontend::TokenKind::Shl:
    case Volt::Frontend::TokenKind::Shr:
    case Volt::Frontend::TokenKind::Amp:
    case Volt::Frontend::TokenKind::Pipe:
    case Volt::Frontend::TokenKind::Caret:
    case Volt::Frontend::TokenKind::Tilde:
    // The three word-spelled operators. Sema's IsOperatorName already accepts
    // them (it takes anything not starting with a letter or '_', plus these
    // three by name), so leaving them out here meant `a and b` could never be
    // given a type: no declaration could exist to carry the signature.
    case Volt::Frontend::TokenKind::KwAnd:
    case Volt::Frontend::TokenKind::KwOr:
    case Volt::Frontend::TokenKind::KwNot:
        return true;
    default:
        return false;
    }
}

} // namespace

bool Volt::Frontend::Parser::AtDeclaration () const
{
    switch ( PeekKind() )
    {
    case TokenKind::KwModule:
    case TokenKind::KwClass:
    case TokenKind::KwStruct:
    case TokenKind::KwMixin:
    case TokenKind::KwDef:
    case TokenKind::KwMacro:
    case TokenKind::KwAbstract:
    case TokenKind::KwExternal:
    case TokenKind::KwInclude:
    case TokenKind::KwComponent:
    case TokenKind::KwCircuit:
    case TokenKind::KwEnum:
        return true;
    case TokenKind::At:
        return PeekKind( 1 ) == TokenKind::LBracket;
    default:
        return false;
    }
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseDeclaration ()
{
    switch ( PeekKind() )
    {
    case TokenKind::KwModule:
        return ParseModule();
    case TokenKind::KwClass:
        return ParseClass();
    case TokenKind::KwStruct:
        return ParseStruct();
    case TokenKind::KwEnum:
        return ParseEnum();
    case TokenKind::KwMixin:
        return ParseMixin();
    case TokenKind::KwDef:
        return ParseMethod( false, false );
    case TokenKind::KwMacro:
        return ParseMacro();
    case TokenKind::KwAbstract:
        Advance();
        return ParseMethod( true, false );
    case TokenKind::KwExternal:
        Advance();
        return ParseMethod( false, true );
    case TokenKind::KwInclude:
        return ParseInclude();
    case TokenKind::KwComponent:
        return ParseComponent();
    case TokenKind::KwCircuit:
        return ParseCircuit();
    case TokenKind::At:
        return ParseAnnotation();
    default:
        ReportHere( "expected a declaration" );
        Advance();
        return DeclId{};
    }
}

Volt::Frontend::SymbolList Volt::Frontend::Parser::ParseGenericParams ()
{
    SymbolList Generics;
    // `class Vector<T> < Base`: the first `<` is glued to the class name
    // and opens the parameter list, the second is spaced and introduces
    // the superclass. AtGenericOpen is what keeps the two apart.
    if ( AtGenericOpen() )
    {
        Advance(); // '<'
        do
        {
            Generics.PushBack( InternText( Expect( TokenKind::Constant, "as a generic parameter" ) ) );
        } while ( Accept( TokenKind::Comma ) );
        ExpectGenericClose( "to close generic parameters" );
    }
    return Generics;
}

void Volt::Frontend::Parser::ParseDeclBlock ( DeclList &Out )
{
    SkipTerminators();
    while ( not AtEnd() and !Check( TokenKind::KwEnd ) )
    {
        const std::size_t Before = Pos;

        DeclId Decl;
        if ( AtDeclaration() )
        {
            Decl = ParseDeclaration();
        }
        else if ( Check( TokenKind::Identifier ) )
        {
            // `name( ... )` in declaration position is a macro invocation;
            // a field never opens a parenthesis after its name.
            Decl = PeekKind( 1 ) == TokenKind::LParen ? ParseMacroInvoke() : ParseFieldOrMember();
        }
        else
        {
            ReportHere( "expected a member declaration" );
        }

        if ( Decl.IsValid() )
        {
            Out.PushBack( Decl );
        }
        DrainAnnotations( Out );
        if ( Pos == Before )
        {
            Advance();
        }
        SkipTerminators();
    }
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseModule ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwModule, "to begin a module" );

    Module Node;
    Node.Name = InternText( Expect( TokenKind::Constant, "as a module name" ) );
    SkipTerminators();
    ParseDeclBlock( Node.Body );
    Expect( TokenKind::KwEnd, "to close module" );

    return MakeDecl( Node, RangeSince( Begin ) );
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseClass ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwClass, "to begin a class" );

    Class Node;
    Node.Name     = InternText( Expect( TokenKind::Constant, "as a class name" ) );
    Node.Generics = ParseGenericParams();
    if ( Accept( TokenKind::Lt ) )
    {
        Node.Super = ParseType();
    }
    SkipTerminators();
    ParseDeclBlock( Node.Body );
    Expect( TokenKind::KwEnd, "to close class" );

    return MakeDecl( Node, RangeSince( Begin ) );
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseStruct ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwStruct, "to begin a struct" );

    Struct Node;
    Node.Name     = InternText( Expect( TokenKind::Constant, "as a struct name" ) );
    Node.Generics = ParseGenericParams();
    SkipTerminators();
    ParseDeclBlock( Node.Body );
    Expect( TokenKind::KwEnd, "to close struct" );

    return MakeDecl( Node, RangeSince( Begin ) );
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseEnum ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwEnum, "to begin an enum" );

    Enum Node;
    Node.Name     = InternText( Expect( TokenKind::Constant, "as an enum name" ) );
    Node.Generics = ParseGenericParams();
    if ( Accept( TokenKind::Colon ) )
    {
        Node.Underlying = ParseType();
    }
    SkipTerminators();
    ParseEnumBody( Node.Body );
    Expect( TokenKind::KwEnd, "to close enum" );

    return MakeDecl( Node, RangeSince( Begin ) );
}

void Volt::Frontend::Parser::ParseEnumBody ( DeclList &Out )
{
    SkipTerminators();
    while ( not AtEnd() and !Check( TokenKind::KwEnd ) )
    {
        const std::size_t Before = Pos;

        DeclId Decl;
        if ( Check( TokenKind::Constant ) )
        {
            Decl = ParseEnumCase();
        }
        else if ( AtDeclaration() )
        {
            Decl = ParseDeclaration();
        }
        else
        {
            ReportHere( "expected an enum case or a member declaration" );
        }

        if ( Decl.IsValid() )
        {
            Out.PushBack( Decl );
        }
        DrainAnnotations( Out );
        if ( Pos == Before )
        {
            Advance();
        }
        SkipTerminators();
    }
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseEnumCase ()
{
    const std::uint32_t Begin = Here();

    EnumCase Node;
    Node.Name = InternText( Expect( TokenKind::Constant, "as an enum case name" ) );

    if ( Accept( TokenKind::LParen ) )
    {
        ParseParameterList( Node.Payload );
        Expect( TokenKind::RParen, "to close an enum case payload" );
    }
    if ( Accept( TokenKind::Assign ) )
    {
        Node.Value = ParseExpr( 0 );
    }

    return MakeDecl( Node, RangeSince( Begin ) );
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseMixin ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwMixin, "to begin a mixin" );

    Mixin Node;
    Node.Name     = InternText( Expect( TokenKind::Constant, "as a mixin name" ) );
    Node.Generics = ParseGenericParams();
    SkipTerminators();
    ParseDeclBlock( Node.Body );
    Expect( TokenKind::KwEnd, "to close mixin" );

    return MakeDecl( Node, RangeSince( Begin ) );
}

void Volt::Frontend::Parser::ParseParameterList ( ParamList &Out )
{
    SkipNewlines();
    if ( Check( TokenKind::RParen ) )
    {
        return;
    }

    do
    {
        SkipNewlines();
        Param Node;
        if ( Accept( TokenKind::Amp ) )
        {
            Node.bIsBlock = true;
            Node.Name     = InternText( Expect( TokenKind::Identifier, "as a block parameter name" ) );
        }
        else if ( Check( TokenKind::InstanceVar ) )
        {
            Node.bInstanceVar        = true;
            const std::string_view V = Interner.Resolve( Advance().Lexeme );
            Node.Name                = Interner.Intern( V.substr( 1 ) ); // strip '@'
        }
        else
        {
            Node.Name = InternText( Expect( TokenKind::Identifier, "as a parameter name" ) );
        }

        if ( Accept( TokenKind::Colon ) )
        {
            Node.DeclType = ParseType();
        }
        if ( Accept( TokenKind::Assign ) )
        {
            Node.Default = ParseExpr( 0 );
        }
        Out.PushBack( Context.Add( Node ) );
        SkipNewlines();
    } while ( Accept( TokenKind::Comma ) );
    SkipNewlines();
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseMethod ( bool bAbstract, bool bExternal )
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwDef, "to begin a method" );

    Method Node;
    Node.bAbstract = bAbstract;
    Node.bExternal = bExternal;

    if ( Accept( TokenKind::KwSelf ) )
    {
        Node.bSelf = true;
        Expect( TokenKind::Dot, "after 'self' in a method name" );
    }

    // Method name: identifier, or an operator method like `[]`, `[]=`,
    // `<=>`, `+`, ... A trailing `=` makes it a setter (`value=`), the
    // named counterpart of `[]=`.
    if ( Check( TokenKind::Identifier ) or Check( TokenKind::Constant ) )
    {
        Node.Name = InternText( Advance() );
        if ( Accept( TokenKind::Assign ) )
        {
            Node.Name = Interner.Intern( std::string( Interner.Resolve( Node.Name ) ) + "=" );
        }
    }
    else if ( Accept( TokenKind::LBracket ) )
    {
        Expect( TokenKind::RBracket, "in '[]' method name" );
        Node.Name = Interner.Intern( Accept( TokenKind::Assign ) ? "[]=" : "[]" );
    }
    else if ( IsOperatorMethodStart( PeekKind() ) )
    {
        Node.Name = Interner.Intern( TokenSpelling( Advance().Kind ) );
    }
    else
    {
        ReportHere( "expected a method name" );
    }

    // After the name, so an operator method is safe: `def <( other )` has
    // already consumed its `<` as the name by the time we look for one
    // opening a parameter list.
    Node.Generics = ParseGenericParams();

    if ( Accept( TokenKind::LParen ) )
    {
        ParseParameterList( Node.Params );
        Expect( TokenKind::RParen, "to close parameters" );
    }

    if ( Accept( TokenKind::Arrow ) )
    {
        Node.ReturnType = ParseType();
    }

    // `abstract` and `external` are the two body-less forms: the first has
    // no implementation yet, the second has one outside Volt.
    if ( not bAbstract and not bExternal )
    {
        SkipTerminators();
        StmtList MainBody;
        ParseStatementBlock( MainBody );

        if ( Check( TokenKind::KwRescue ) or Check( TokenKind::KwEnsure ) )
        {
            const std::uint32_t BeginBody = Here();
            BeginExpr BeginNode;
            BeginNode.Body = MainBody;
            ParseRescueEnsure( BeginNode );
            Expect( TokenKind::KwEnd, "to close method" );
            const ExprId BeginExprId = MakeExpr( BeginNode, RangeSince( BeginBody ) );
            ExprStmt StmtNode;
            StmtNode.Expr = BeginExprId;
            Node.Body.PushBack( MakeStmt( StmtNode, RangeSince( BeginBody ) ) );
        }
        else
        {
            Node.Body = MainBody;
            Expect( TokenKind::KwEnd, "to close method" );
        }
    }

    return MakeDecl( Node, RangeSince( Begin ) );
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseFieldOrMember ()
{
    const std::uint32_t Begin = Here();

    EAccessor Accessor = EAccessor::None;
    if ( Check( TokenKind::Identifier ) and PeekKind( 1 ) == TokenKind::Identifier )
    {
        const std::string_view Word = Spelling( Peek() );
        if ( Word == "getter" )
        {
            Accessor = EAccessor::Getter;
            Advance();
        }
        else if ( Word == "property" )
        {
            Accessor = EAccessor::Property;
            Advance();
        }
    }

    Field Node;
    Node.Accessor = Accessor;
    Node.Name     = InternText( Expect( TokenKind::Identifier, "as a field name" ) );
    if ( Accept( TokenKind::Colon ) )
    {
        Node.DeclType = ParseType();
    }
    if ( Accept( TokenKind::Assign ) )
    {
        Node.Default = ParseExpr( 0 );
    }
    return MakeDecl( Node, RangeSince( Begin ) );
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseMacro ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwMacro, "to begin a macro" );
    Expect( TokenKind::KwDef, "after 'macro'" );

    MacroDef Node;
    Node.Name = InternText( Expect( TokenKind::Identifier, "as a macro name" ) );
    if ( Accept( TokenKind::LParen ) )
    {
        ParseParameterList( Node.Params );
        Expect( TokenKind::RParen, "to close macro parameters" );
    }
    SkipTerminators();

    // The body is not parsed: capture the raw source slice up to the
    // matching `end`, tracking block-opener nesting at token level.
    const std::uint32_t BodyBegin = Here();
    std::uint32_t BodyEnd         = BodyBegin;
    int Depth                     = 0;
    while ( not AtEnd() )
    {
        const TokenKind Kind = PeekKind();
        if ( IsBlockOpener( Kind ) )
        {
            ++Depth;
        }
        else if ( Kind == TokenKind::KwEnd )
        {
            if ( Depth == 0 )
            {
                break;
            }
            --Depth;
        }
        BodyEnd = Advance().Range.End;
    }
    Node.BodyText = Interner.Intern( Source.substr( BodyBegin, BodyEnd - BodyBegin ) );
    Expect( TokenKind::KwEnd, "to close macro" );

    return MakeDecl( Node, RangeSince( Begin ) );
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseMacroInvoke ()
{
    const std::uint32_t Begin = Here();

    MacroInvoke Node;
    Node.Name = InternText( Expect( TokenKind::Identifier, "as a macro name" ) );
    Expect( TokenKind::LParen, "to open macro arguments" );
    ParseCallArguments( Node.Args, Node.ArgNames, TokenKind::RParen );
    Expect( TokenKind::RParen, "to close macro arguments" );

    return MakeDecl( Node, RangeSince( Begin ) );
}

void Volt::Frontend::Parser::ParseMemberBlock ( DeclList &Out )
{
    ParseDeclBlock( Out );
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseInclude ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwInclude, "to begin an include" );

    Include Node;
    Node.Target = ParseType();
    return MakeDecl( Node, RangeSince( Begin ) );
}

void Volt::Frontend::Parser::DrainAnnotations ( DeclList &Out )
{
    for ( const DeclId Id : AnnotationOverflow )
    {
        Out.PushBack( Id );
    }
    AnnotationOverflow.Clear();
}

void Volt::Frontend::Parser::DrainAnnotations ( std::vector<DeclId> &Out )
{
    for ( const DeclId Id : AnnotationOverflow )
    {
        Out.push_back( Id );
    }
    AnnotationOverflow.Clear();
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseAnnotation ()
{
    // The group's `@[` anchors the first entry, so a lone `@[Name]` keeps the
    // exact range it always had; later entries anchor on their own name.
    const std::uint32_t GroupBegin = Here();
    Expect( TokenKind::At, "to begin an annotation" );
    Expect( TokenKind::LBracket, "to open an annotation" );

    // A group is a comma-separated list: `@[Primitive( "i32", 32 ), Literal( IntLiteral )]`.
    // Each entry becomes its own Annotation decl; the first is returned and
    // the rest wait in AnnotationOverflow for the caller's drain.
    DeclId First;
    do
    {
        const std::uint32_t EntryBegin = First.IsValid() ? Here() : GroupBegin;

        Annotation Node;
        if ( Check( TokenKind::Constant ) or Check( TokenKind::Identifier ) )
        {
            Node.Name = InternText( Advance() );
        }
        else
        {
            ReportHere( "expected an annotation name" );
        }

        if ( Accept( TokenKind::LParen ) )
        {
            SymbolList Ignored;
            ParseCallArguments( Node.Args, Ignored, TokenKind::RParen );
            Expect( TokenKind::RParen, "to close annotation arguments" );
        }

        const DeclId Entry = MakeDecl( Node, RangeSince( EntryBegin ) );
        if ( First.IsValid() )
        {
            AnnotationOverflow.PushBack( Entry );
        }
        else
        {
            First = Entry;
        }
    } while ( Accept( TokenKind::Comma ) );

    Expect( TokenKind::RBracket, "to close annotation" );

    return First;
}

Volt::Frontend::DeclId Volt::Frontend::Parser::ParseCircuit ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwCircuit, "to begin a circuit" );

    Circuit Node;
    Node.Name = InternText( Expect( TokenKind::StringLiteral, "as a circuit name" ) );
    SkipNewlines();
    Expect( TokenKind::LBrace, "to open a circuit body" );
    SkipTerminators();

    while ( not AtEnd() and !Check( TokenKind::RBrace ) )
    {
        const std::size_t Before = Pos;
        const StmtId Stmt        = ParseStatement();
        if ( Stmt.IsValid() )
        {
            Node.Body.PushBack( Stmt );
        }
        if ( Pos == Before )
        {
            Advance();
        }
        SkipTerminators();
    }
    Expect( TokenKind::RBrace, "to close a circuit body" );

    return MakeDecl( Node, RangeSince( Begin ) );
}
