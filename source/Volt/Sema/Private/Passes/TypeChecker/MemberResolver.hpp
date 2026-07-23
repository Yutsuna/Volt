#pragma once

#include "TypeCheckerContext.hpp"

#include <string_view>

namespace Volt::Sema::TypeCheckerPass
{

constexpr std::string_view IndexOperator = "[]";

// `T.new( ... )` is spelled one way and declared another: the call site says
// `new`, the body says `initialize`. Both are Volt *syntax*, not type names,
// so naming them here is not a hardcoded type.
constexpr std::string_view ConstructorCall = "new";
constexpr std::string_view ConstructorName = "initialize";

[[nodiscard]] Resolution LookupOn ( TypeCheckerContext &Context, SemaTypeId Receiver, std::string_view Name );

// Calling a value directly — `f( x )` where `f` is not a method name but a
// callable — invokes whatever member its type declares as `@[Apply]`. Which
// member that is comes from the annotation, so no member name is ever known
// to the compiler; a type that declares none is simply not callable.
[[nodiscard]] Resolution LookupApplyOn ( TypeCheckerContext &Context, SemaTypeId Receiver );

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

void CheckMemberSelf ( TypeCheckerContext &Context, Core::SourceRange Loc, const Resolution &Found, bool bReceiverIsNakedType );

void CheckDotCallSelf ( TypeCheckerContext &Context, Core::SourceRange Loc, const Resolution &Found );

// True when `Name` is an operator the backend supplies directly on a type
// whose layout is primitive or pointer. Volt declares those members as
// abstract contracts (`mixin Arithmetic`) but never writes their bodies —
// the spelling of the layout is what selects the machine instruction. The
// unknown-member diagnostic and the abstract-conformance check must honour
// the same exemption, or one contradicts the other.
[[nodiscard]] bool IsBuiltinOpOn ( const TypeCheckerContext &Context, NominalId Base, std::string_view Name );

[[nodiscard]] SemaTypeId MemberType (
    TypeCheckerContext &Context, Core::SourceRange Loc, SemaTypeId Receiver, bool bReceiverIsNakedType, std::string_view Name );

void CheckCallArgs ( TypeCheckerContext &Context,
                     Core::SourceRange Loc,
                     const Resolution &Found,
                     const Frontend::ExprList &Args );

void CheckArity ( TypeCheckerContext &Context, Core::SourceRange Loc, NominalId Base, std::size_t Given );

} // namespace Volt::Sema::TypeCheckerPass
