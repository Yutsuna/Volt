#include "Volt/BackendCore/InstanceLayout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <utility>
#include <variant>

namespace
{

// A cyclic `include` or a malformed signature must not hang codegen. The same
// bound TypeStore::LookupMember uses, for the same reason.
constexpr std::uint32_t MaxDepth = 16;

// Length of the MonoRequest-encoded subtree starting at `Offset`: its own two
// header words plus every child's length. Zero when the span is truncated.
[[nodiscard]] std::size_t SubtreeLength ( std::span<const std::uint32_t> Flat, std::size_t Offset )
{
    if ( Offset + 1 >= Flat.size() )
    {
        return 0;
    }

    const std::uint32_t Count = Flat[Offset + 1];
    std::size_t Length        = 2;
    for ( std::uint32_t Index = 0; Index < Count; ++Index )
    {
        const std::size_t Child = SubtreeLength( Flat, Offset + Length );
        if ( Child == 0 )
        {
            return 0;
        }
        Length += Child;
    }
    return Length;
}

// Append the MonoRequest encoding of one *concrete* type: the signature `Id`
// with `FlatArgs` answering its generic parameter references. False when the
// signature names something the bindings cannot answer — a hole the caller
// reports rather than papers over.
[[nodiscard]] bool FlattenSig ( const Volt::Sema::TypeStore &Store,
                                Volt::Sema::SigTypeId Id,
                                std::span<const std::uint32_t> FlatArgs,
                                std::uint32_t Depth,
                                std::vector<std::uint32_t> &Out )
{
    if ( not Id.IsValid() or Depth > MaxDepth )
    {
        return false;
    }

    const Volt::Sema::SigType &Sig = Store.Sig( Id );

    // A generic parameter is answered verbatim by the binding that filled it:
    // the argument arrives already concrete, so its subtree is copied as-is.
    if ( Sig.ParamIndex >= 0 )
    {
        const std::span<const std::uint32_t> Subtree =
            Volt::Backend::ArgSubtree( FlatArgs, static_cast<std::size_t>( Sig.ParamIndex ) );
        if ( Subtree.empty() )
        {
            return false;
        }
        Out.insert( Out.end(), Subtree.begin(), Subtree.end() );
        return true;
    }

    // `self` in a field signature would need the enclosing instance, which is
    // a shape no aggregate can hold by value anyway. Refused, not guessed.
    if ( not Sig.Base.IsValid() )
    {
        return false;
    }

    Out.push_back( Sig.Base.Value );
    Out.push_back( static_cast<std::uint32_t>( Sig.Args.Size() ) );
    for ( const Volt::Sema::SigTypeId Arg : Sig.Args )
    {
        if ( not FlattenSig( Store, Arg, FlatArgs, Depth + 1, Out ) )
        {
            return false;
        }
    }
    return true;
}

} // namespace

std::span<const std::uint32_t> Volt::Backend::ArgSubtree ( std::span<const std::uint32_t> FlatArgs, std::size_t Index )
{
    std::size_t Offset = 0;
    for ( std::size_t Current = 0; Offset < FlatArgs.size(); ++Current )
    {
        const std::size_t Length = SubtreeLength( FlatArgs, Offset );
        if ( Length == 0 )
        {
            break;
        }
        if ( Current == Index )
        {
            return FlatArgs.subspan( Offset, Length );
        }
        Offset += Length;
    }
    return {};
}

Volt::Sema::LayoutId Volt::Backend::InstanceLayouts::OfSignature ( Sema::TypeStore &Store,
                                                                   Sema::SigTypeId Id,
                                                                   std::span<const std::uint32_t> FlatArgs )
{
    if ( not Id.IsValid() )
    {
        return Sema::LayoutId{};
    }

    if ( const Sema::NominalId Base = Store.Sig( Id ).Base; Base.IsValid() )
    {
        if ( const Sema::LayoutId Attached = Store.Type( Base ).Layout; Attached.IsValid() )
        {
            return Attached;
        }
    }
    return OfSig( Store, Id, FlatArgs, 1 );
}

Volt::Sema::LayoutId Volt::Backend::InstanceLayouts::OfSig ( Sema::TypeStore &Store,
                                                             Sema::SigTypeId Id,
                                                             std::span<const std::uint32_t> FlatArgs,
                                                             std::uint32_t Depth )
{
    std::vector<std::uint32_t> Tree;
    if ( Depth > MaxDepth or not FlattenSig( Store, Id, FlatArgs, Depth, Tree ) or Tree.size() < 2 )
    {
        return Sema::LayoutId{};
    }

    const Sema::NominalId Base{ Tree[0] };
    // Tree[1] is the argument count; everything after the header *is* that
    // node's argument list, in the same encoding.
    return Of( Store, Base, std::span<const std::uint32_t>{ Tree }.subspan( 2 ) );
}

bool Volt::Backend::InstanceLayouts::IsCallable ( const Sema::TypeStore &Store, Sema::NominalId Base )
{
    // One type claims all three node kinds today (`Proc`), but nothing here
    // depends on that: each is asked for separately, exactly as the literal
    // protocol does.
    constexpr std::array<std::string_view, 3> Kinds{ "FuncType", "Lambda", "Block" };
    return std::ranges::any_of( Kinds,
                                [&Store, Base] ( const std::string_view NodeKind )
                                {
                                    const auto Claimant = Store.LookupNodeKind( NodeKind );
                                    return Claimant.has_value() and *Claimant == Base;
                                } );
}

Volt::Sema::LayoutId Volt::Backend::InstanceLayouts::ClosurePair ( Sema::TypeStore &Store )
{
    if ( Pair.IsValid() )
    {
        return Pair;
    }

    // Two addresses: the code to enter, and the environment to enter it with.
    // Both are `Pointer` rather than an `@[Primitive("ptr")]` spelling, so the
    // pair's size follows the target's pointer size through LayoutEngine — the
    // wasm encoding of an address is four bytes, and this shape must follow it.
    Sema::Aggregate Shape;
    Shape.Fields.PushBack( Sema::FieldLayout{ .Name = Store.Intern( "code" ), .Type = Store.AddPointer( Sema::LayoutId{} ) } );
    Shape.Fields.PushBack( Sema::FieldLayout{ .Name = Store.Intern( "env" ), .Type = Store.AddPointer( Sema::LayoutId{} ) } );

    Pair = Store.AddAggregate( std::move( Shape ) );
    return Pair;
}

Volt::Sema::LayoutId
Volt::Backend::InstanceLayouts::Of ( Sema::TypeStore &Store, Sema::NominalId Base, std::span<const std::uint32_t> FlatArgs )
{
    if ( not Base.IsValid() )
    {
        return Sema::LayoutId{};
    }

    std::vector<std::uint32_t> Key;
    Key.reserve( 1 + FlatArgs.size() );
    Key.push_back( Base.Value );
    Key.insert( Key.end(), FlatArgs.begin(), FlatArgs.end() );

    if ( const auto It = Cache.find( Key ); It != Cache.end() )
    {
        return It->second;
    }

    // An already-attached layout wins over any substitution: `@[Primitive]`
    // fixes a shape regardless of arguments — `Pointer<T>` is `ptr` for every
    // T — and a non-generic aggregate was already computed by TypeBinder.
    if ( const Sema::LayoutId Attached = Store.Type( Base ).Layout; Attached.IsValid() )
    {
        Cache.emplace( std::move( Key ), Attached );
        return Attached;
    }

    // A callable's shape is the one thing the stdlib cannot write down: the
    // type claiming FuncType/Lambda/Block declares no field, because `{ code,
    // env }` is an ABI decision, fixed once for the three targets in
    // .agents/backend/abi.md. Materialising it here — rather than in one
    // emitter — is what keeps a closure written by native codegen readable by
    // the VM's and wasm's rules.
    if ( IsCallable( Store, Base ) )
    {
        const Sema::LayoutId Shape = ClosurePair( Store );
        Cache.emplace( std::move( Key ), Shape );
        return Shape;
    }

    // Claim the key before descending: a generic that reaches itself through a
    // by-value field is malformed, and must terminate as an invalid layout
    // rather than recurse forever.
    const auto [Slot, bFresh] = Cache.emplace( Key, Sema::LayoutId{} );
    if ( not bFresh )
    {
        return Slot->second;
    }

    Sema::Aggregate Shape;

    // The base's fields lead, flattened — the same shape TypeBinder gives a
    // non-generic class, and for the same reason: a method the base declares
    // GEPs its own fields at its own offsets through this very pointer, and an
    // inherited `@x` is looked up by name in the subclass's layout. Here the
    // parent link is a *signature*, so it goes through OfSig and its arguments
    // are substituted out of FlatArgs exactly like a field's would be.
    if ( const Sema::LayoutId Inherited = OfSig( Store, Store.Type( Base ).Super, FlatArgs, 1 ); Inherited.IsValid() )
    {
        if ( const auto *Parent = std::get_if<Sema::Aggregate>( &Store.Get( Inherited ) ); Parent != nullptr )
        {
            for ( const Sema::FieldLayout &Field : Parent->Fields )
            {
                Shape.Fields.PushBack( Field );
            }
        }
    }

    for ( const Sema::Member &Entry : Store.Type( Base ).Members )
    {
        if ( Entry.Kind != Sema::EMemberKind::Field )
        {
            continue;
        }
        Shape.Fields.PushBack( Sema::FieldLayout{ .Name = Entry.Name, .Type = OfSig( Store, Entry.Result, FlatArgs, 1 ) } );
    }

    if ( Shape.Fields.Size() == 0 )
    {
        return Sema::LayoutId{};
    }

    const Sema::LayoutId Built = Store.AddAggregate( std::move( Shape ) );

    // std::map nodes are stable, so the iterator claimed above is still the
    // slot to fill — no second lookup.
    Slot->second = Built;
    return Built;
}
