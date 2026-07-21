#include "Volt/Sema/Link/InterfaceRegistry.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace Volt
{

namespace Sema
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

} // namespace Sema

} // namespace Volt
