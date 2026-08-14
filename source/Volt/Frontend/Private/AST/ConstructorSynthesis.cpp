#include "Volt/Frontend/AST/ConstructorSynthesis.hpp"

#include "Volt/Frontend/AST/Decl.hpp"

#include <algorithm>
#include <string_view>
#include <utility>
#include <variant>

namespace
{

using namespace Volt;

[[nodiscard]] bool HasMethodNamed ( const Frontend::AstContext &Context, const Frontend::DeclList &Body, std::string_view Name )
{
    return std::ranges::any_of( Body,
                                [&] ( const Frontend::DeclId Child )
                                {
                                    const auto *Existing =
                                        Child.IsValid() ? std::get_if<Frontend::Method>( &Context.Decl( Child ) ) : nullptr;
                                    return Existing != nullptr and Context.Text( Existing->Name ) == Name;
                                } );
}

template <typename T> void EnsureInitializeMethod ( Frontend::AstContext &Context, Frontend::DeclId Id, T Type )
{
    if ( HasMethodNamed( Context, Type.Body, "initialize" ) )
    {
        return;
    }

    Frontend::Method Ctor;
    Ctor.Loc  = Type.Loc;
    Ctor.Name = Context.Strings().Intern( "initialize" );
    Type.Body.PushBack( Context.Add( Ctor ) );
    Context.Decl( Id ) = Frontend::DeclNode{ std::move( Type ) };
}

} // namespace

void Volt::Frontend::SynthesizeDefaultConstructors ( AstContext &Context )
{
    const std::size_t OriginalDeclCount = Context.DeclCount();

    for ( std::size_t Index = 0; Index < OriginalDeclCount; ++Index )
    {
        const DeclId Id{ static_cast<DeclId::ValueType>( Index ) };
        const DeclNode &Node = Context.Decl( Id );

        if ( const auto *ClassDecl = std::get_if<Frontend::Class>( &Node ) )
        {
            EnsureInitializeMethod( Context, Id, *ClassDecl );
        }
        else if ( const auto *StructDecl = std::get_if<Frontend::Struct>( &Node ) )
        {
            EnsureInitializeMethod( Context, Id, *StructDecl );
        }
    }
}
