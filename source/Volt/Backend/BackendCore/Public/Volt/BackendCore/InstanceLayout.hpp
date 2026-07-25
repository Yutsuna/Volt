#pragma once

// InstanceLayout.hpp — the memory shape of a *monomorphised* generic.
//
// TypeBinder deliberately leaves NominalType::Layout invalid for a generic:
// "only computable for a non-generic whose field types are already bound".
// `Array<T>` has no shape until T is known, and T becomes known at
// instantiation, which is codegen (rules/core-ast.md). This is where the shape
// is finally materialised.
//
// This is *materialisation, not inference*. No type is decided here: the
// bindings arrive already fixed by Sema (CalleeEntry::Bindings, flattened into
// a MonoRequest), and all this does is walk the declared field signatures and
// substitute. A backend that called this to *discover* a type would be doing
// semantic analysis, which the architecture forbids.
//
// Thread-safety: this grows the store's layout arena, so it must run on one
// thread. That is true of codegen by construction, and it does not disturb the
// serial-binder freeze: layouts live in their own arena, so adding one cannot
// invalidate the `Member *` / `NominalType &` handles Sema handed out.

#include "BackendCore_export.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace Volt
{

namespace Backend
{

    // Memoised instantiation of layouts, one instance per build.
    //
    // Keys are `Base` followed by the argument encoding MonoRequest uses — a
    // pre-order walk emitting, per node, its NominalId value then its own
    // argument count. One currency shared with Monomorphizer and Mangler, so a
    // layout, a symbol and a queue entry can never disagree about which
    // instantiation they mean.
    class BACKENDCORE_EXPORT InstanceLayouts
    {

    public:

        // The layout of `Base` instantiated with `FlatArgs`.
        //
        // Three cases, in order, none of which reads a Volt type name:
        //   - the nominal already carries a layout (`@[Primitive]`, or a
        //     non-generic aggregate TypeBinder could already compute) — use it,
        //     arguments and all: `Pointer<T>` is `ptr` for every T;
        //   - the nominal claims a closure node kind (`FuncType` / `Lambda` /
        //     `Block`) — the `{ code, env }` pair abi.md fixes for all three
        //     targets, which no stdlib declaration could express;
        //   - a generic aggregate — substitute into its declared fields and
        //     build the aggregate here;
        //   - nothing resolvable — an invalid LayoutId, which the caller
        //     reports as a middle-end contract violation rather than guessing.
        [[nodiscard]] Sema::LayoutId Of ( Sema::TypeStore &Store, Sema::NominalId Base, std::span<const std::uint32_t> FlatArgs );

        // The layout a *declared signature* resolves to — a parameter, a
        // result, a field — with `FlatArgs` answering whatever generic
        // parameters it mentions.
        //
        // An attached layout short-circuits the substitution entirely, by the
        // same rule Of() states: `@[Primitive]` fixes a shape whatever the
        // arguments are. That is not an optimisation but the only correct
        // reading of `Pointer<Void>` — the argument names a type the stdlib
        // never declares, yet the pointer's shape does not depend on it.
        [[nodiscard]] Sema::LayoutId
        OfSignature ( Sema::TypeStore &Store, Sema::SigTypeId Id, std::span<const std::uint32_t> FlatArgs );

        [[nodiscard]] std::size_t InstantiationCount () const
        {
            return Cache.size();
        }

    private:

        // OfSignature's recursive half, carrying the depth bound.
        [[nodiscard]] Sema::LayoutId
        OfSig ( Sema::TypeStore &Store, Sema::SigTypeId Id, std::span<const std::uint32_t> FlatArgs, std::uint32_t Depth );

        // Is this the type a written signature, a lambda and a trailing block
        // all denote? Asked of the store through `@[Literal]`, the same
        // mechanism that identifies the type behind `nil` or a string literal
        // — no Volt type name enters (rules/zero-hardcode.md).
        [[nodiscard]] static bool IsCallable ( const Sema::TypeStore &Store, Sema::NominalId Base );

        // `{ code, env }`, memoised. A callable's arity and result live in its
        // *type*, never in its memory shape, so one layout serves every
        // instantiation — which is why this is not keyed on the arguments.
        [[nodiscard]] Sema::LayoutId ClosurePair ( Sema::TypeStore &Store );

        std::map<std::vector<std::uint32_t>, Sema::LayoutId> Cache;
        Sema::LayoutId Pair;
    };

    // The `Index`-th top-level argument subtree of a MonoRequest-encoded
    // argument list, as its own encoded span. Returns an empty span when the
    // list holds fewer arguments than that.
    //
    // Free rather than a member because the same slicing is what a
    // monomorphising emitter needs when it substitutes into a nested generic.
    [[nodiscard]] BACKENDCORE_EXPORT std::span<const std::uint32_t> ArgSubtree ( std::span<const std::uint32_t> FlatArgs,
                                                                                 std::size_t Index );

} // namespace Backend

} // namespace Volt
