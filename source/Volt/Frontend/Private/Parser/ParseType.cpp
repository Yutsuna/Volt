#include "Volt/Frontend/Parser/Parser.hpp"

#include <cstdint>

namespace Volt
{

    namespace Frontend
    {

        bool Parser::AtTypeStart() const
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

        TypeId Parser::ParseTypePrimary()
        {
            const std::uint32_t Begin = Here();

            TypeRef Ref;
            if ( !Check( TokenKind::Constant ) )
            {
                ReportHere( "expected a type name" );
                return MakeType( TypeRef{}, RangeSince( Begin ) );
            }

            Ref.Path.PushBack( InternText( Advance() ) );
            while ( Accept( TokenKind::ColonColon ) )
            {
                Ref.Path.PushBack( InternText( Expect( TokenKind::Constant, "in qualified type name" ) ) );
            }

            // Generic arguments `[T, U]` — but `[<int>]` is a fixed-array size,
            // handled as a postfix in ParseType, so only recurse when the
            // bracket opens on something type-shaped.
            if ( Check( TokenKind::LBracket ) &&
                 ( PeekKind( 1 ) == TokenKind::Constant || PeekKind( 1 ) == TokenKind::Arrow || PeekKind( 1 ) == TokenKind::LParen ) )
            {
                Advance();
                do
                {
                    Ref.Generics.PushBack( ParseType() );
                } while ( Accept( TokenKind::Comma ) );
                Expect( TokenKind::RBracket, "to close generic arguments" );
            }

            return MakeType( std::move( Ref ), RangeSince( Begin ) );
        }

        TypeId Parser::ParseType()
        {
            const std::uint32_t Begin = Here();

            // Function type with no explicit parameters: `-> Ret`.
            if ( Accept( TokenKind::Arrow ) )
            {
                FuncType Func;
                Func.Return = ParseType();
                return MakeType( std::move( Func ), RangeSince( Begin ) );
            }

            // Parenthesised: either `(A, B) -> R` or a grouped `(T)`.
            if ( Accept( TokenKind::LParen ) )
            {
                TypeList Params;
                if ( !Check( TokenKind::RParen ) )
                {
                    do
                    {
                        Params.PushBack( ParseType() );
                    } while ( Accept( TokenKind::Comma ) );
                }
                Expect( TokenKind::RParen, "to close parameter types" );

                if ( Accept( TokenKind::Arrow ) )
                {
                    FuncType Func;
                    Func.Params = std::move( Params );
                    Func.Return = ParseType();
                    return MakeType( std::move( Func ), RangeSince( Begin ) );
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
                    Base        = MakeType( std::move( Ptr ), RangeSince( Begin ) );
                }
                else if ( Accept( TokenKind::Question ) )
                {
                    NilableType Nil;
                    Nil.Inner = Base;
                    Base      = MakeType( std::move( Nil ), RangeSince( Begin ) );
                }
                else if ( Check( TokenKind::LBracket ) && PeekKind( 1 ) == TokenKind::IntLiteral )
                {
                    Advance(); // '['
                    FixedArrayType Fixed;
                    Fixed.Elem = Base;
                    Fixed.Size = ParseExpr();
                    Expect( TokenKind::RBracket, "to close fixed-array size" );
                    Base = MakeType( std::move( Fixed ), RangeSince( Begin ) );
                }
                else
                {
                    break;
                }
            }

            return Base;
        }

    }

}
