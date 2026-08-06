#pragma once

#include "Frontend_export.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

namespace Volt
{

namespace Frontend
{

    // Materializes every `EnumCase`'s implicit ordinal (`Red = 0`, `Green =
    // 1`, ...) as a synthesized `IntLiteral` assigned to `EnumCase::Value` —
    // an explicit value (`Ok = 200`) is left untouched, and resets the
    // running counter to `Value + 1` for the cases that follow. Purely
    // syntactic, no type information needed.
    //
    // Runs from Driver::ParseOne, before Sema::BindUnitTypes — same seam as
    // `LowerPointFreeDefs`, and for a similar reason: TypeBinder's Phase A
    // (`Sema/Private/Layout/TypeBinder.cpp`) caches each case's ordinal onto
    // its `Member` (`Member::EnumOrdinal`) the moment it declares the
    // member, so the value must already exist in the AST by then — earlier
    // than any Sema pass (this used to be one, `EnumLowering`, order 12)
    // ever runs.
    FRONTEND_EXPORT void MaterializeEnumOrdinals ( AstContext &Context );

    // A payload-less enum (`Color`, `TaskStatus` — no case carries a
    // payload) gets a synthesized `to_value` method returning its own tag/
    // underlying value: `self` already *is* that value once `TypeBinder`
    // gives the enum a `Primitive` layout (`EnsureEnumLayout`,
    // `Sema/Private/Layout/TypeBinder.cpp`), exactly like `Symbol#hash`
    // (`source/Lib/Primitives/Symbol.vl`) returns `self` verbatim.
    //
    // Runs from Driver::ParseOne, immediately after parsing and before
    // Sema::BindUnitTypes — same seam as `LowerPointFreeDefs`, and for the
    // identical reason: a member appended after that seam would never be
    // found by `TypeBinder`'s Phase A/B, since those run first and freeze
    // each type's member list.
    //
    // The synthesized method's return type is left unwritten on purpose —
    // there is no Volt type name to spell at parse time for an enum with no
    // `: Underlying` (it defaults to whichever nominal claims `IntLiteral`,
    // a Sema-time-only fact). `TypeBinder`'s Phase B resolves it specially,
    // the same way it resolves the enum's own tag layout.
    FRONTEND_EXPORT void SynthesizeEnumMembers ( AstContext &Context );

} // namespace Frontend

} // namespace Volt
