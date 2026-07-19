#pragma once

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace Volt
{

namespace Frontend
{

    namespace Detail
    {

        template <typename> struct IsNodeList : std::false_type
        {
        };

        template <typename T, std::size_t N> struct IsNodeList<Core::SmallVec<T, N>> : std::true_type
        {
        };

        [[nodiscard]] constexpr std::string_view AccessorName ( EAccessor Accessor )
        {
            switch ( Accessor )
            {
            case EAccessor::None:
                return "none";
            case EAccessor::Getter:
                return "getter";
            case EAccessor::Property:
                return "property";
            }
            return "?";
        }

    } // namespace Detail

    /// Reflective S-expression printer. It knows nothing about individual
    /// nodes: it walks the manifest-generated variant and each struct's
    /// fields via P2996, so a new node prints correctly with zero printer code.
    class AstPrinter
    {

    public:

        AstPrinter ( const AstContext &InContext, std::ostream &InOut ) : Context( InContext ), Out( InOut )
        {
        }

        void PrintFile ()
        {
            for ( const DeclId Id : Context.TopDecls )
            {
                Print( Id, 0 );
                Out << '\n';
            }
            for ( const StmtId Id : Context.TopStmts )
            {
                Print( Id, 0 );
                Out << '\n';
            }
        }

        void Print ( ExprId Id, int Depth )
        {
            PrintVariant( Context.Expr( Id ), Depth );
        }

        void Print ( StmtId Id, int Depth )
        {
            PrintVariant( Context.Stmt( Id ), Depth );
        }

        void Print ( DeclId Id, int Depth )
        {
            PrintVariant( Context.Decl( Id ), Depth );
        }

        void Print ( TypeId Id, int Depth )
        {
            PrintVariant( Context.Type( Id ), Depth );
        }

        void Print ( ParamId Id, int Depth )
        {
            PrintStruct( "Param", Context.GetParam( Id ), Depth );
        }

    private:

        void NewlineIndent ( int Depth )
        {
            Out << '\n' << std::string( static_cast<std::size_t>( Depth ) * 2, ' ' );
        }

        template <typename Variant> void PrintVariant ( const Variant &Node, int Depth )
        {
            Out << '(' << NodeName( Node );
            std::visit( Meta::Overloaded{ [] ( const std::monostate & ) {},
                                          [&] ( const auto &Struct ) { PrintFieldsOf( Struct, Depth ); } },
                        Node );
            Out << ')';
        }

        template <typename Struct> void PrintStruct ( std::string_view Name, const Struct &Value, int Depth )
        {
            Out << '(' << Name;
            PrintFieldsOf( Value, Depth );
            Out << ')';
        }

        template <typename Struct> void PrintFieldsOf ( const Struct &Value, int Depth )
        {
            Meta::ForEachField( Value,
                                [&] ( const char *FieldName, const auto &FieldValue )
                                {
                                    NewlineIndent( Depth + 1 );
                                    Out << FieldName << '=';
                                    PrintValue( FieldValue, Depth + 1 );
                                } );
        }

        template <typename F> void PrintValue ( const F &Value, int Depth )
        {
            if constexpr ( std::same_as<F, Symbol> )
            {
                Out << '\'' << ( Value.IsValid() ? Context.Strings().Resolve( Value ) : std::string_view{} ) << '\'';
            }
            else if constexpr ( std::same_as<F, ExprId> or std::same_as<F, StmtId> or std::same_as<F, DeclId> ||
                                std::same_as<F, TypeId> or std::same_as<F, ParamId> )
            {
                if ( Value.IsValid() )
                {
                    Print( Value, Depth );
                }
                else
                {
                    Out << "nil";
                }
            }
            else if constexpr ( std::same_as<F, TokenKind> )
            {
                const std::string_view Spelling = TokenSpelling( Value );
                Out << ( Spelling.empty() ? TokenName( Value ) : Spelling );
            }
            else if constexpr ( std::same_as<F, bool> )
            {
                Out << ( Value ? "true" : "false" );
            }
            else if constexpr ( std::same_as<F, EAccessor> )
            {
                Out << Detail::AccessorName( Value );
            }
            else if constexpr ( Detail::IsNodeList<F>::value )
            {
                Out << '[';
                for ( std::size_t Index = 0; Index < Value.Size(); ++Index )
                {
                    NewlineIndent( Depth + 1 );
                    PrintValue( Value[Index], Depth + 1 );
                }
                if ( !Value.IsEmpty() )
                {
                    NewlineIndent( Depth );
                }
                Out << ']';
            }
            else if constexpr ( std::is_enum_v<F> )
            {
                Out << Meta::EnumName( Value );
            }
            else
            {
                Out << Value;
            }
        }

        const AstContext &Context;
        std::ostream &Out;
    };

} // namespace Frontend

} // namespace Volt
