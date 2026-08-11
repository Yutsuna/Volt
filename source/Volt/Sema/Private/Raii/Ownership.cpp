#include "Ownership.hpp"

#include "Volt/Sema/Layout/TypeResolve.hpp"

#include <optional>

namespace Volt::Sema::Raii
{

bool IsFinalizeCandidateNominal ( const TypeStore &Store, const NominalId Id )
{
    if ( not Id.IsValid() )
    {
        return false;
    }
    const NominalType &Type = Store.Type( Id );
    if ( not Type.Layout.IsValid() or KindOf( Store.Get( Type.Layout ) ) != LayoutKind::Aggregate )
    {
        return false;
    }
    return Store.LookupMember( Id, FinalizeName ).Decl != nullptr;
}

bool IsFinalizeCandidateType ( const TypeStore &Store, UnitTypes &Values, const SemaTypeId Type )
{
    if ( not Type.IsValid() or not Values.Has( Type ) )
    {
        return false;
    }
    const NominalId Base = Values.Get( Type ).Base;
    if ( not Base.IsValid() )
    {
        return false;
    }
    const NominalType &Nominal = Store.Type( Base );
    if ( not Nominal.Layout.IsValid() or KindOf( Store.Get( Nominal.Layout ) ) != LayoutKind::Aggregate )
    {
        return false;
    }
    return LookupMemberOn( Store, Values, Type, Type, FinalizeName ).Decl != nullptr;
}

// The gate is the sequence type's own node-kind claim — `@[Literal( ArrayLit )]`
// — read back through `LookupNodeKind`, exactly as `nil` is identified through
// `NilLiteral` (TypeCompat.cpp), a pointee through `PointerType` and the
// callable type through `FuncType` (ExprInferencer.cpp). A node kind is the
// compiler's own vocabulary, so naming one here spells no Volt type name
// (rules/zero-hardcode.md's "a node-kind claim answers it").
//
// Gating on "has a generic argument that declares finalize" instead is a build
// breaker rather than a missed free, because the cascade this feeds
// synthesizes `receiver.size` and `receiver[ i ]` calls and so must only ever
// fire on a receiver that actually declares those:
//
//   - the callable type's own argument is its *return* type, not an element.
//     A String-returning closure typed as a sequence the moment that type
//     gained a `finalize` of its own (393032b), and every file holding one
//     stopped compiling with `type Proc has no member 'size'`
//     (samples/Tests/Functional/{Curry,PointFree}.vl).
//   - the hash type's first argument is its *key* type, so the loop would
//     index a hash by integer position.
//
// `IsSubclassOf` rather than an identity test, so a user type deriving from
// the sequence type cascades too.
SemaTypeId ContainerElementType ( const TypeStore &Store, UnitTypes &Values, const SemaTypeId Type )
{
    if ( not Type.IsValid() or not Values.Has( Type ) )
    {
        return SemaTypeId{};
    }
    const SemaType &Value = Values.Get( Type );
    if ( Value.Args.IsEmpty() or not Value.Base.IsValid() )
    {
        return SemaTypeId{};
    }

    const std::optional<NominalId> SequenceBase = Store.LookupNodeKind( "ArrayLit" );
    if ( not SequenceBase.has_value() or not IsSubclassOf( Store, Value.Base, *SequenceBase ) )
    {
        return SemaTypeId{};
    }

    const SemaTypeId ElementType = Value.Args[0];
    if ( IsFinalizeCandidateType( Store, Values, ElementType ) )
    {
        return ElementType;
    }
    return SemaTypeId{};
}

} // namespace Volt::Sema::Raii
