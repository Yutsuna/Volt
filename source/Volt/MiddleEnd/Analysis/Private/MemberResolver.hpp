#pragma once

#include "Volt/Core/Diagnostics/SourceLocation.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"

#include <string_view>

namespace Volt::MiddleEnd::Analysis
{

using TypeStore   = Volt::MiddleEnd::TypeSystem::TypeStore;
using SourceRange = ::Volt::Core::SourceRange;

constexpr std::string_view IndexOperator = "[]";

// `T.new( ... )` is spelled one way and declared another: the call site says
// `new`, the body says `initialize`. Both are Volt *syntax*, not type names,
// so naming them here is not a hardcoded type.
constexpr std::string_view ConstructorCall = "new";
constexpr std::string_view ConstructorName = "initialize";

// The RAII-style destructor spelling used to be declared here *and*, verbatim,
// in TypeBinder.cpp — the two sites sit on opposite sides of a directory
// boundary and neither could include the other's private header. It now has a
// single definition, `MiddleEnd::Analysis::Raii::FinalizeName` (Private/Raii/Ownership.hpp),
// which both include.

[[nodiscard]] Resolution LookupOn ( TypeCheckerContext &Context, SemaTypeId Receiver, std::string_view Name );

// A top-level `def`, looked up by name alone — no receiver, so no `self` to
// substitute and no owner arguments to instantiate against. Reuses the same
// Resolution / Reinstantiate machinery LookupOn does, since TypeStore models
// a free function as a receiver-less Member.
[[nodiscard]] Resolution LookupFreeFunction ( TypeCheckerContext &Context, std::string_view Name );

// Whether `Receiver` is *the* callable type — the one the stdlib declares
// with `@[Literal( FuncType )]`. That claim is the only fact needed: a
// written signature, a lambda and a block all denote one thing, so one type
// wraps the three node kinds and nothing else can be invoked.
//
// This is the same identification `TypeCompat` makes for `nil`
// (`LookupNodeKind( "NilLiteral" )`) and `ExprInferencer` for a pointee
// (`LookupNodeKind( "PointerType" )`) — a *node kind* is the compiler's own
// vocabulary, unlike a Volt type name (rules/zero-hardcode.md).
[[nodiscard]] bool IsCallableType ( const TypeCheckerContext &Context, SemaTypeId Receiver );

// Whether `Receiver` is a dynamic polymorphic fat pointer type (`@[Literal( DynamicType )]`).
[[nodiscard]] bool IsDynamicType ( const TypeCheckerContext &Context, SemaTypeId Receiver );

// If TargetType is Dynamic<Trait> and ValExpr is a concrete type, wraps ValExpr in DynamicUpcast.
[[nodiscard]] Frontend::ExprId CoerceToDynamic ( TypeCheckerContext &Context, Frontend::ExprId ValExpr, SemaTypeId TargetType );

// Does the nominal collapse to a Primitive or Pointer layout, so that abstract
// operations on it are supplied by the machine/backend rather than by a body?
[[nodiscard]] bool IsMachineSuppliedOn ( const TypeCheckerContext &Context, TypeSystem::NominalId Base );

// Calling a value directly — `f( x )` where `f` is not a method name but a
// callable. The member invoked is the callable type's one abstract contract,
// found by walking its members rather than by spelling `call` in C++.
[[nodiscard]] Resolution LookupCallOn ( TypeCheckerContext &Context, SemaTypeId Receiver );

// Recompute a resolution's result, parameters and block slot from its
// current Bindings. Idempotent, and called again each time inference
// closes one more generic hole.
void Reinstantiate ( TypeCheckerContext &Context, Resolution &Found );

// Bind the method's own generics from the positional arguments, then
// recompute. A no-op for a method that declares none, which is nearly all
// of them.
void UnifyArgs ( TypeCheckerContext &Context, Resolution &Found, const Frontend::ExprList &Args );

// Bind the method's own generics from the trailing block's actual type,
// then recompute. This is what makes `arr.map do | i | i > 1 end` an
// Array<Bool>.
void UnifyBlock ( TypeCheckerContext &Context, Resolution &Found, SemaTypeId BlockType );

void CheckMemberSelf ( TypeCheckerContext &Context, SourceRange Loc, const Resolution &Found, bool bReceiverIsNakedType );

// May the code being checked *see* what `Found` resolved to? `private` is
// reachable only from the body of the type that declares it, `protected` from
// that type and everything below it — the `TypeSystem::ConformsTo` relation,
// shared with assignability so that "a type I may be used as" and "a type
// whose protected members I reach" cannot drift. Anything unwritten is public.
//
// The comparison is against `Context.SelfType`, the type whose body we are
// inside, and never against the receiver: that is what makes `other.balance`
// legal between two instances of the same class and illegal everywhere else,
// which is the C++/Crystal rule rather than Ruby's implicit-receiver one.
//
// Called from the two places a member is reached by name — MemberType, which
// covers `x.f`, `@f` and every operator, and ExprInferencer's bare-identifier
// branch, which is the implicit-`self` call `f` resolves through. Both already
// record the resolution, so the check sits next to the recording rather than
// inside LookupOn, which re-resolves and would report the same access twice.
void CheckMemberAccess ( TypeCheckerContext &Context, SourceRange Loc, const Resolution &Found );

// Whether `Nominal` is an enum in the only sense the compiler ever needs to
// know: it declares at least one case of its own. A plain `case x when 1, 2
// end` over a non-enum value must never trip exhaustiveness, and a struct
// with no cases must never be exempted from writing its own operators — so
// this is the shared gate both the exhaustiveness check and the operator
// exemption below rely on.
[[nodiscard]] bool HasEnumCases ( const TypeStore &Store, NominalId Nominal );

// True when `Name` is an operator the backend supplies directly on a type
// whose layout is primitive or pointer, *or* the structural `===` an enum's
// desugared `when Enum::Case` pattern (`CaseLowering`) compares its
// discriminant with. Volt declares those members as abstract contracts
// (`mixin Arithmetic`) but never writes their bodies — the spelling of the
// layout (or, for an enum, the case set itself) is what selects the
// comparison. The unknown-member diagnostic and the abstract-conformance
// check must honour the same exemption, or one contradicts the other.
[[nodiscard]] bool IsBuiltinOpOn ( const TypeCheckerContext &Context, NominalId Base, std::string_view Name );

// Resolve `Name` on `Receiver` for the expression `Id`, diagnose an unknown
// member, and — the part backends depend on — **record the resolution** in
// `Context.CalleeResolution[Id.Value]`.
//
// Recording here rather than at each call site is what makes the operator
// contract of `rules/core-ast.md` hold: `Binary` and `Unary` are core nodes on
// a primitive layout only, and on any other layout they *are* method calls.
// A backend distinguishes the two by reading this map, so an operator whose
// resolution was resolved and then dropped would force every backend to redo
// member lookup. `Id` replaces the old `Loc` parameter — the location is
// derived from the node, so the two can no longer disagree.
[[nodiscard]] SemaTypeId MemberType (
    TypeCheckerContext &Context, Frontend::ExprId Id, SemaTypeId Receiver, bool bReceiverIsNakedType, std::string_view Name );

void CheckCallArgs ( TypeCheckerContext &Context, SourceRange Loc, const Resolution &Found, const Frontend::ExprList &Args );

void CheckArity ( TypeCheckerContext &Context, SourceRange Loc, NominalId Base, std::size_t Given );

} // namespace Volt::MiddleEnd::Analysis
