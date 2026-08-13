#include "Volt/BackendCore/InstanceLayout.hpp"

#include <cstddef>
#include <string>
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
// with `FlatArgs` answering its generic parameter references and `SelfArgs`
// (`SelfSubtree`'s encoding of the receiver) answering any nested `self`
// (`Comparable#..`'s `-> Range<self>`, not just a bare `-> self`). False when
// the signature names something the bindings cannot answer — a hole the
// caller reports rather than papers over.
[[nodiscard]] bool FlattenSig ( const Volt::MiddleEnd::TypeSystem::TypeStore &Store,
                                Volt::MiddleEnd::TypeSystem::SigTypeId Id,
                                std::span<const std::uint32_t> FlatArgs,
                                std::span<const std::uint32_t> SelfArgs,
                                std::uint32_t Depth,
                                std::vector<std::uint32_t> &Out )
{
    if ( not Id.IsValid() or Depth > MaxDepth )
    {
        return false;
    }

    const Volt::MiddleEnd::TypeSystem::SigType &Sig = Store.Sig( Id );

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

    // `self`, wherever it is nested, means the receiver itself — answered by
    // `SelfArgs` exactly as a generic parameter is answered by `FlatArgs`.
    // Refused, not guessed, when the caller had no receiver to offer one
    // (a bare field signature can never mention `self` to begin with).
    if ( Sig.ParamIndex == Volt::MiddleEnd::TypeSystem::SigType::SelfParam )
    {
        if ( SelfArgs.empty() )
        {
            return false;
        }
        Out.insert( Out.end(), SelfArgs.begin(), SelfArgs.end() );
        return true;
    }

    if ( not Sig.Base.IsValid() )
    {
        return false;
    }

    Out.push_back( Sig.Base.Value );
    Out.push_back( static_cast<std::uint32_t>( Sig.Args.Size() ) );
    for ( const Volt::MiddleEnd::TypeSystem::SigTypeId Arg : Sig.Args )
    {
        if ( not FlattenSig( Store, Arg, FlatArgs, SelfArgs, Depth + 1, Out ) )
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

std::vector<std::uint32_t> Volt::Backend::SelfSubtree ( const MiddleEnd::TypeSystem::TypeStore &Store,
                                                        MiddleEnd::TypeSystem::NominalId Base,
                                                        std::span<const std::uint32_t> FlatArgs )
{
    std::vector<std::uint32_t> Out;
    if ( not Base.IsValid() )
    {
        return Out;
    }
    Out.push_back( Base.Value );
    Out.push_back( static_cast<std::uint32_t>( Store.Type( Base ).Params.Size() ) );
    Out.insert( Out.end(), FlatArgs.begin(), FlatArgs.end() );
    return Out;
}

Volt::MiddleEnd::TypeSystem::LayoutId Volt::Backend::InstanceLayouts::OfSignature ( MiddleEnd::TypeSystem::TypeStore &Store,
                                                                                    MiddleEnd::TypeSystem::SigTypeId Id,
                                                                                    std::span<const std::uint32_t> FlatArgs,
                                                                                    std::span<const std::uint32_t> SelfArgs )
{
    if ( not Id.IsValid() )
    {
        return MiddleEnd::TypeSystem::LayoutId{};
    }

    if ( const MiddleEnd::TypeSystem::NominalId Base = Store.Sig( Id ).Base; Base.IsValid() )
    {
        if ( const MiddleEnd::TypeSystem::LayoutId Attached = Store.Type( Base ).Layout; Attached.IsValid() )
        {
            return Attached;
        }
    }
    return OfSig( Store, Id, FlatArgs, SelfArgs, 1 );
}

Volt::MiddleEnd::TypeSystem::LayoutId Volt::Backend::InstanceLayouts::OfSig ( MiddleEnd::TypeSystem::TypeStore &Store,
                                                                              MiddleEnd::TypeSystem::SigTypeId Id,
                                                                              std::span<const std::uint32_t> FlatArgs,
                                                                              std::span<const std::uint32_t> SelfArgs,
                                                                              std::uint32_t Depth )
{
    std::vector<std::uint32_t> Tree;
    if ( Depth > MaxDepth or not FlattenSig( Store, Id, FlatArgs, SelfArgs, Depth, Tree ) or Tree.size() < 2 )
    {
        return MiddleEnd::TypeSystem::LayoutId{};
    }

    const MiddleEnd::TypeSystem::NominalId Base{ Tree[0] };
    // Tree[1] is the argument count; everything after the header *is* that
    // node's argument list, in the same encoding.
    return Of( Store, Base, std::span<const std::uint32_t>{ Tree }.subspan( 2 ) );
}

Volt::MiddleEnd::TypeSystem::LayoutId Volt::Backend::InstanceLayouts::Of ( MiddleEnd::TypeSystem::TypeStore &Store,
                                                                           MiddleEnd::TypeSystem::NominalId Base,
                                                                           std::span<const std::uint32_t> FlatArgs )
{
    if ( not Base.IsValid() )
    {
        return MiddleEnd::TypeSystem::LayoutId{};
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
    if ( const MiddleEnd::TypeSystem::LayoutId Attached = Store.Type( Base ).Layout; Attached.IsValid() )
    {
        Cache.emplace( std::move( Key ), Attached );
        return Attached;
    }

    // Claim the key before descending: a generic that reaches itself through a
    // by-value field is malformed, and must terminate as an invalid layout
    // rather than recurse forever.
    const auto [Slot, bFresh] = Cache.emplace( Key, MiddleEnd::TypeSystem::LayoutId{} );
    if ( not bFresh )
    {
        return Slot->second;
    }

    MiddleEnd::TypeSystem::Aggregate Shape;

    // `self` inside any of Base's own field/parent signatures means this
    // very instance — `Base` instantiated with `FlatArgs` — the same subtree
    // `Of()` is already building for the cache key.
    const std::vector<std::uint32_t> SelfArgs = SelfSubtree( Store, Base, FlatArgs );

    // The base's fields lead, flattened — the same shape TypeBinder gives a
    // non-generic class, and for the same reason: a method the base declares
    // GEPs its own fields at its own offsets through this very pointer, and an
    // inherited `@x` is looked up by name in the subclass's layout. Here the
    // parent link is a *signature*, so it goes through OfSig and its arguments
    // are substituted out of FlatArgs exactly like a field's would be.
    if ( const MiddleEnd::TypeSystem::LayoutId Inherited = OfSig( Store, Store.Type( Base ).Super, FlatArgs, SelfArgs, 1 );
         Inherited.IsValid() )
    {
        if ( const auto *Parent = std::get_if<MiddleEnd::TypeSystem::Aggregate>( &Store.Get( Inherited ) ); Parent != nullptr )
        {
            for ( const MiddleEnd::TypeSystem::FieldLayout &Field : Parent->Fields )
            {
                Shape.Fields.PushBack( Field );
            }
        }
    }

    // A generic enum (`Optional<T>`) never gets a `Field`-only pass here:
    // its tag needs no substitution (it's never generic), and each payload
    // param substitutes exactly like a field's declared type would — reread
    // through `Entry.Params`, since an `EnumCase` member's own `Result` is
    // the constructed enum itself (`SelfSigOf`), not a payload type.
    bool bHasEnumCase = false;
    for ( const MiddleEnd::TypeSystem::Member &Entry : Store.Type( Base ).Members )
    {
        if ( Entry.Kind == MiddleEnd::TypeSystem::EMemberKind::EnumCase )
        {
            bHasEnumCase = true;
            break;
        }
    }

    if ( bHasEnumCase )
    {
        // The tag never depends on a generic argument (see
        // `EnumTagNominal`/`EnsureEnumLayout`, `TypeBinder.cpp`) — a
        // non-generic enum's own already-attached `Primitive` layout is
        // exactly what a generic enum's tag reuses too, found the same way
        // `TypeBinder` finds it for the non-generic case: whichever nominal
        // claims `IntLiteral`, unless the enum wrote `: Underlying`. Neither
        // is expressible from a bare `TypeStore` without the AST, so instead
        // this reads the width straight off any one `EnumCase` member that
        // has already been resolved for a *non-generic* sibling enum — which
        // doesn't exist here. Simplify: every generic enum's tag is the
        // same default `IntLiteral`-claiming primitive a non-generic enum
        // with no `: Underlying` gets; a generic enum writing `: Underlying`
        // is out of scope until a sample needs it.
        if ( const auto DefaultTag = Store.LookupNodeKind( "IntLiteral" ) )
        {
            if ( const MiddleEnd::TypeSystem::LayoutId TagLayout = Store.Type( *DefaultTag ).Layout; TagLayout.IsValid() )
            {
                Shape.Fields.PushBack( MiddleEnd::TypeSystem::FieldLayout{ .Name = Store.Intern( "tag" ), .Type = TagLayout } );
            }
        }

        for ( const MiddleEnd::TypeSystem::Member &Entry : Store.Type( Base ).Members )
        {
            if ( Entry.Kind != MiddleEnd::TypeSystem::EMemberKind::EnumCase )
            {
                continue;
            }
            // Same naming rule as `TypeBinder::EnsureEnumLayout` (`$` +
            // case name alone for a single payload, `$<CaseName>_<Index>`
            // past that) — kept in sync by hand since a bare `TypeStore`
            // has no AST to re-derive the written parameter name from. The
            // `$` matters as much here as there: an `EnumCase` member of
            // the identical bare name already lives in this same Members
            // vector, and `OwnMember`'s first-match lookup would otherwise
            // resolve `self.Some` to it instead of this field.
            for ( std::size_t Index = 0; Index < Entry.Params.Size(); ++Index )
            {
                const std::string FieldName = Entry.Params.Size() > 1
                                                  ? "$" + std::string{ Store.Text( Entry.Name ) } + "_" + std::to_string( Index )
                                                  : "$" + std::string{ Store.Text( Entry.Name ) };
                Shape.Fields.PushBack( MiddleEnd::TypeSystem::FieldLayout{
                    .Name = Store.Intern( FieldName ), .Type = OfSig( Store, Entry.Params[Index], FlatArgs, SelfArgs, 1 ) } );
            }
        }
    }
    else
    {
        for ( const MiddleEnd::TypeSystem::Member &Entry : Store.Type( Base ).Members )
        {
            if ( Entry.Kind != MiddleEnd::TypeSystem::EMemberKind::Field )
            {
                continue;
            }
            Shape.Fields.PushBack( MiddleEnd::TypeSystem::FieldLayout{
                .Name = Entry.Name, .Type = OfSig( Store, Entry.Result, FlatArgs, SelfArgs, 1 ) } );
        }
    }

    if ( Shape.Fields.Size() == 0 )
    {
        return MiddleEnd::TypeSystem::LayoutId{};
    }

    const MiddleEnd::TypeSystem::LayoutId Built = Store.AddAggregate( std::move( Shape ) );

    // std::map nodes are stable, so the iterator claimed above is still the
    // slot to fill — no second lookup.
    Slot->second = Built;
    return Built;
}
