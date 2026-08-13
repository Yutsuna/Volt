#include "Volt/BackendCore/Mangler.hpp"

#include <cstddef>
#include <string_view>

namespace
{

// `<len><text>` — the whole scheme is this one primitive, applied to type and
// member spellings alike.
void AppendLengthPrefixed ( std::string &Out, std::string_view Text )
{
    Out += std::to_string( Text.size() );
    Out += Text;
}

// One node of a MonoRequest-encoded argument tree, plus its own arguments.
// `Cursor` walks the flat vector; a malformed tail simply stops the walk
// rather than reading past the end — a truncated key is a caller bug, not
// something to crash on while producing a symbol name.
void AppendArg ( std::string &Out,
                 const Volt::MiddleEnd::TypeSystem::TypeStore &Store,
                 std::span<const std::uint32_t> FlatArgs,
                 std::size_t &Cursor )
{
    if ( Cursor + 1 >= FlatArgs.size() )
    {
        Cursor = FlatArgs.size();
        return;
    }

    const Volt::MiddleEnd::TypeSystem::NominalId Base{ FlatArgs[Cursor] };
    const std::uint32_t Count = FlatArgs[Cursor + 1];
    Cursor += 2;

    Out += Volt::Backend::MangleNominal( Store, Base );

    if ( Count == 0 )
    {
        return;
    }

    Out += 'I';
    for ( std::uint32_t Index = 0; Index < Count and Cursor < FlatArgs.size(); ++Index )
    {
        AppendArg( Out, Store, FlatArgs, Cursor );
    }
    Out += 'E';
}

} // namespace

std::string Volt::Backend::MangleNominal ( const MiddleEnd::TypeSystem::TypeStore &Store, MiddleEnd::TypeSystem::NominalId Id )
{
    std::string Out;
    if ( not Id.IsValid() )
    {
        // A missing nominal still has to occupy its slot, or two different
        // instantiations could mangle to the same string.
        AppendLengthPrefixed( Out, std::string_view{} );
        return Out;
    }
    AppendLengthPrefixed( Out, Store.Text( Store.Type( Id ).Name ) );
    return Out;
}

std::string Volt::Backend::MangleFunction ( const MiddleEnd::TypeSystem::TypeStore &Store,
                                            const MiddleEnd::TypeSystem::Member &Entry,
                                            MiddleEnd::TypeSystem::NominalId Owner,
                                            std::span<const std::uint32_t> FlatArgs )
{
    // The C boundary: `@[External( "libc", "malloc" )]` means the linker and a
    // C compiler must agree on the spelling, so there is nothing to mangle.
    if ( Entry.ExternSymbol.IsValid() )
    {
        return std::string{ Store.Text( Entry.ExternSymbol ) };
    }

    std::string Out = "_V";
    Out += MangleNominal( Store, Owner );
    AppendLengthPrefixed( Out, Store.Text( Entry.Name ) );

    if ( FlatArgs.empty() )
    {
        return Out;
    }

    Out += 'I';
    for ( std::size_t Cursor = 0; Cursor < FlatArgs.size(); )
    {
        AppendArg( Out, Store, FlatArgs, Cursor );
    }
    Out += 'E';
    return Out;
}
