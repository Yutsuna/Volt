#include "Volt/BackendCore/LayoutEngine.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"

#include <algorithm>
#include <bit>
#include <variant>

namespace
{

constexpr std::size_t PointerSize = 8;

[[nodiscard]] constexpr std::size_t AlignTo ( std::size_t Offset, std::size_t Alignment )
{
    return ( Offset + Alignment - 1 ) & ~( Alignment - 1 );
}

[[nodiscard]] constexpr Volt::Backend::SizeAlign PrimitiveSizeAlign ( std::uint32_t Bits )
{
    const std::size_t Bytes = std::max<std::size_t>( 1, ( Bits + 7 ) / 8 );
    return { .Size = Bytes, .Alignment = std::min<std::size_t>( std::bit_ceil( Bytes ), PointerSize ) };
}

} // namespace

Volt::Backend::SizeAlign Volt::Backend::LayoutEngine::Of ( MiddleEnd::TypeSystem::LayoutId Id ) const
{
    if ( not Id.IsValid() )
    {
        return {};
    }

    return std::visit( Meta::Overloaded{ [] ( const std::monostate & ) { return SizeAlign{}; },
                                         [] ( const MiddleEnd::TypeSystem::Primitive &Node )
                                         { return PrimitiveSizeAlign( Node.Bits ); },
                                         [] ( const MiddleEnd::TypeSystem::Pointer & )
                                         { return SizeAlign{ .Size = PointerSize, .Alignment = PointerSize }; },
                                         [this] ( const MiddleEnd::TypeSystem::Aggregate &Node )
                                         {
                                             SizeAlign Result;
                                             for ( const MiddleEnd::TypeSystem::FieldLayout &Field : Node.Fields )
                                             {
                                                 const SizeAlign Inner = Of( Field.Type );
                                                 Result.Size           = AlignTo( Result.Size, Inner.Alignment ) + Inner.Size;
                                                 Result.Alignment      = std::max( Result.Alignment, Inner.Alignment );
                                             }
                                             Result.Size = AlignTo( Result.Size, Result.Alignment );
                                             return Result;
                                         } },
                       Store.Get( Id ) );
}

std::size_t Volt::Backend::LayoutEngine::FieldOffset ( MiddleEnd::TypeSystem::LayoutId Aggregate, std::size_t Index ) const
{
    if ( not Aggregate.IsValid() )
    {
        return 0;
    }

    const auto *Node = std::get_if<MiddleEnd::TypeSystem::Aggregate>( &Store.Get( Aggregate ) );
    if ( Node == nullptr or Index >= Node->Fields.Size() )
    {
        return 0;
    }

    std::size_t Offset = 0;
    for ( std::size_t Field = 0; Field <= Index; ++Field )
    {
        const SizeAlign Inner = Of( Node->Fields[Field].Type );
        Offset                = AlignTo( Offset, Inner.Alignment );
        if ( Field == Index )
        {
            return Offset;
        }
        Offset += Inner.Size;
    }
    return Offset;
}
