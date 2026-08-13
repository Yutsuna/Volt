#include "Volt/Frontend/Parser/Parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

Volt::Frontend::TypeId Volt::Frontend::Parser::FinishFuncType ( TypeList Params, std::uint32_t Begin )
{
    FuncType Func;
    Func.Params = std::move( Params );
    Func.Return = ParseType();
    return MakeType( Func, RangeSince( Begin ) );
}

bool Volt::Frontend::Parser::AtGenericOpen () const
{
    // A `<` welded to the preceding token opens a generic list; a spaced
    // `<` is comparison / superclass (`a < b`, `class Vector<T> < Base`).
    return Check( TokenKind::Lt ) and Pos > 0 and Tokens[Pos - 1].Range.End == Peek().Range.Begin;
}

bool Volt::Frontend::Parser::Parser::AtGenericArgs () const
{
    // The type list must start on a type name — in Volt those are always
    // Constants — which already rejects `count<max`. `self` and `typeof` are
    // the two non-Constant spellings a type can start with (ParseTypePrimary
    // treats both as a type primary), the first needed for `Range<self>.new(
    // ...)` inside a generic-body method like `Comparable#..`, the second for
    // `Vector<typeof( x )>.new`.
    if ( not AtGenericOpen() or ( PeekKind( 1 ) != TokenKind::Constant and PeekKind( 1 ) != TokenKind::KwSelf and
                                  PeekKind( 1 ) != TokenKind::KwTypeOf ) )
    {
        return false;
    }

    // Scan for the matching `>`, allowing only tokens a type list can
    // contain. Anything else (an operator, a literal, a newline, EOF)
    // means this was a comparison after all: `x<Foo && y>z`.
    int Depth = 0;
    for ( std::size_t Ahead = 0;; ++Ahead )
    {
        switch ( PeekKind( Ahead ) )
        {
        case TokenKind::Lt:
            ++Depth;
            break;
        case TokenKind::Gt:
            if ( --Depth == 0 )
            {
                return true;
            }
            break;
        case TokenKind::Shr: // `>>` closes two levels at once
            Depth -= 2;
            if ( Depth <= 0 )
            {
                return true;
            }
            break;
        case TokenKind::KwTypeOf:
        {
            // `Vector<typeof( a + b )>` — the operand is an arbitrary
            // expression, so the conservative token filter cannot vet it and
            // must not try: every token it would reject is legal in there.
            // Skip the balanced parenthesis group wholesale and resume vetting
            // after it. An unbalanced group means this was never a type list.
            if ( PeekKind( Ahead + 1 ) != TokenKind::LParen )
            {
                return false;
            }
            int Parens = 0;
            for ( std::size_t Probe = Ahead + 1;; ++Probe )
            {
                const TokenKind Inner = PeekKind( Probe );
                if ( Inner == TokenKind::LParen )
                {
                    ++Parens;
                }
                else if ( Inner == TokenKind::RParen and --Parens == 0 )
                {
                    Ahead = Probe;
                    break;
                }
                else if ( Inner == TokenKind::Eof )
                {
                    return false;
                }
            }
            break;
        }
        case TokenKind::Constant:
        case TokenKind::KwSelf:
        case TokenKind::ColonColon:
        case TokenKind::Comma:
        case TokenKind::Star:
        case TokenKind::Question:
        case TokenKind::Arrow:
        case TokenKind::LParen:
        case TokenKind::RParen:
            break;
        default:
            return false;
        }
    }
}

bool Volt::Frontend::Parser::Parser::AcceptGenericClose ()
{
    if ( Accept( TokenKind::Gt ) )
    {
        return true;
    }

    // `Pointer<Vector<T>>` lexes its tail as one `>>`. Rewrite the token
    // in place to the second `>` and report the first as consumed, so the
    // enclosing list still finds a `Gt` waiting for it.
    if ( Check( TokenKind::Shr ) )
    {
        Token &Tok = Tokens[Pos];
        Tok.Kind   = TokenKind::Gt;
        Tok.Range.Begin += 1;
        return true;
    }
    return false;
}

void Volt::Frontend::Parser::ExpectGenericClose ( std::string_view Where )
{
    if ( not AcceptGenericClose() )
    {
        ReportHere( "expected '>' " + std::string( Where ) );
    }
}

Volt::Frontend::TypeList Volt::Frontend::Parser::ParseGenericArgs ()
{
    TypeList Args;
    Expect( TokenKind::Lt, "to open generic arguments" );
    do
    {
        Args.PushBack( ParseType() );
    } while ( Accept( TokenKind::Comma ) );
    ExpectGenericClose( "to close generic arguments" );
    return Args;
}

bool Volt::Frontend::Parser::AtTypeStart () const
{
    switch ( PeekKind() )
    {
    case TokenKind::Constant:
    case TokenKind::Arrow:
    case TokenKind::LParen:
        return true;
    default:
        return false;
    }
}

Volt::Frontend::TypeId Volt::Frontend::Parser::ParseTypePrimary ()
{
    const std::uint32_t Begin = Here();

    TypeRef Ref;

    // `typeof( expr )` — a value's type, standing exactly where a type name
    // stands. Parenthesised always: the operand is an expression, so without
    // a closing delimiter `typeof x * 2` has no reading the grammar can pick.
    if ( Accept( TokenKind::KwTypeOf ) )
    {
        TypeOfType Node;
        Expect( TokenKind::LParen, "after 'typeof'" );
        Node.Value = ParseExpr( 0 );
        Expect( TokenKind::RParen, "to close 'typeof'" );
        return MakeType( Node, RangeSince( Begin ) );
    }

    // `-> self` — the enclosing type, spelled with the keyword.
    if ( Check( TokenKind::KwSelf ) )
    {
        Ref.Path.PushBack( InternText( Advance() ) );
        return MakeType( Ref, RangeSince( Begin ) );
    }

    if ( not Check( TokenKind::Constant ) )
    {
        ReportHere( "expected a type name" );
        return MakeType( TypeRef{}, RangeSince( Begin ) );
    }

    Ref.Path.PushBack( InternText( Advance() ) );
    while ( Accept( TokenKind::ColonColon ) )
    {
        Ref.Path.PushBack( InternText( Expect( TokenKind::Constant, "in qualified type name" ) ) );
    }

    // Generic arguments `<T, U>`. Comparison operators cannot appear in a
    // type, so here a `<` is never ambiguous and needs no lookahead.
    if ( Check( TokenKind::Lt ) )
    {
        Ref.Generics = ParseGenericArgs();
    }

    return MakeType( Ref, RangeSince( Begin ) );
}

Volt::Frontend::TypeId Volt::Frontend::Parser::ParseType ()
{
    const std::uint32_t Begin = Here();

    // Function type with no explicit parameters: `-> Ret`.
    if ( Accept( TokenKind::Arrow ) )
    {
        return FinishFuncType( {}, Begin );
    }

    // Parenthesised: either `(A, B) -> R` or a grouped `(T)`.
    if ( Accept( TokenKind::LParen ) )
    {
        TypeList Params;
        if ( not Check( TokenKind::RParen ) )
        {
            do
            {
                Params.PushBack( ParseType() );
            } while ( Accept( TokenKind::Comma ) );
        }
        Expect( TokenKind::RParen, "to close parameter types" );

        if ( Accept( TokenKind::Arrow ) )
        {
            return FinishFuncType( std::move( Params ), Begin );
        }

        if ( Params.Size() == 1 )
        {
            return Params[0];
        }
        ReportAt( RangeSince( Begin ), "expected '->' after parenthesised parameter types" );
        return Params.IsEmpty() ? MakeType( TypeRef{}, RangeSince( Begin ) ) : Params[0];
    }

    TypeId Base = ParseTypePrimary();

    // Postfix type operators.
    for ( ;; )
    {
        if ( Accept( TokenKind::Star ) )
        {
            PointerType Ptr;
            Ptr.Pointee = Base;
            Base        = MakeType( Ptr, RangeSince( Begin ) );
        }
        else if ( Accept( TokenKind::Question ) )
        {
            NilableType Nilable;
            Nilable.Inner = Base;
            Base          = MakeType( Nilable, RangeSince( Begin ) );
        }
        else if ( Check( TokenKind::LBracket ) and PeekKind( 1 ) == TokenKind::IntLiteral )
        {
            Advance(); // '['
            FixedArrayType Fixed;
            Fixed.Elem = Base;
            Fixed.Size = ParseExpr();
            Expect( TokenKind::RBracket, "to close fixed-array size" );
            Base = MakeType( Fixed, RangeSince( Begin ) );
        }
        else
        {
            break;
        }
    }

    // Bare single-parameter function type: `T -> R` (right-associative).
    if ( Accept( TokenKind::Arrow ) )
    {
        TypeList Params;
        Params.PushBack( Base );
        return FinishFuncType( std::move( Params ), Begin );
    }

    return Base;
}
