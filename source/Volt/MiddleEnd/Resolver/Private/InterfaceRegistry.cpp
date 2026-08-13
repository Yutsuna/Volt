#include "Volt/MiddleEnd/Resolver/InterfaceRegistry.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Serialize.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace Volt
{

namespace MiddleEnd::Resolver
{

    namespace
    {

        constexpr std::string_view QualifySeparator = "::";

        // Publish one declaration list under Prefix, recursing through
        // `module` nesting. Only *named type-level* declarations export;
        // fields, includes and annotations stay unit-private. A new
        // exportable decl kind = one lambda line here.
        std::size_t PublishDecls ( const Frontend::AstContext &Ast,
                                   const std::string &Prefix,
                                   const auto &DeclIds,
                                   std::uint32_t Unit,
                                   InterfaceRegistry &Registry )
        {
            std::size_t Published = 0;

            const auto Qualify = [&] ( Frontend::Symbol Name )
            {
                return Prefix.empty() ? std::string{ Ast.Text( Name ) }
                                      : Prefix + std::string{ QualifySeparator } + std::string{ Ast.Text( Name ) };
            };

            const auto Export = [&] ( Frontend::Symbol Name, Frontend::DeclKind Kind, Frontend::DeclId Id )
            {
                Registry.Publish( ExportedDecl{ Qualify( Name ), Kind, Unit, Id } );
                ++Published;
            };

            for ( const Frontend::DeclId Id : DeclIds )
            {
                std::visit(
                    Meta::Overloaded{
                        [&] ( const Frontend::Module &Node )
                        {
                            Export( Node.Name, Frontend::DeclKind::Module, Id );
                            Published += PublishDecls( Ast, Qualify( Node.Name ), Node.Body, Unit, Registry );
                        },
                        [&] ( const Frontend::Class &Node ) { Export( Node.Name, Frontend::DeclKind::Class, Id ); },
                        [&] ( const Frontend::Struct &Node ) { Export( Node.Name, Frontend::DeclKind::Struct, Id ); },
                        [&] ( const Frontend::Mixin &Node ) { Export( Node.Name, Frontend::DeclKind::Mixin, Id ); },
                        [&] ( const Frontend::Component &Node ) { Export( Node.Name, Frontend::DeclKind::Component, Id ); },
                        [&] ( const Frontend::Enum &Node ) { Export( Node.Name, Frontend::DeclKind::Enum, Id ); },
                        [&] ( const Frontend::Method &Node ) { Export( Node.Name, Frontend::DeclKind::Method, Id ); },
                        [] ( const auto & ) {},
                    },
                    Ast.Decl( Id ) );
            }

            return Published;
        }

    } // namespace

    std::size_t PublishUnitInterface ( const Frontend::AstContext &Ast, std::uint32_t Unit, InterfaceRegistry &Registry )
    {
        return PublishDecls( Ast, std::string{}, Ast.TopDecls, Unit, Registry );
    }

    void InterfaceRegistry::SerializeCache ( Meta::Writer &W ) const
    {
        const auto Count = static_cast<std::uint32_t>( Decls.size() );
        Meta::Serialize( W, Count );
        for ( const ExportedDecl &Entry : Decls )
        {
            Meta::Serialize( W, Entry );
        }
    }

    bool InterfaceRegistry::DeserializeCache ( Meta::Reader &R )
    {
        std::uint32_t Count = 0;
        if ( not Meta::Deserialize( R, Count ) )
        {
            return false;
        }
        for ( std::uint32_t Index = 0; Index < Count; ++Index )
        {
            ExportedDecl Entry;
            if ( not Meta::Deserialize( R, Entry ) )
            {
                return false;
            }
            Publish( std::move( Entry ) );
        }
        return true;
    }

} // namespace MiddleEnd::Resolver

} // namespace Volt
