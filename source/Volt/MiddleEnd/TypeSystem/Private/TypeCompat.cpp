// TypeCompat.cpp — the single assignability predicate.
//
// Promoted verbatim from what used to be `ArgTypeMatches`, an anonymous-
// namespace helper in MemberResolver.cpp with exactly one caller
// (`CheckCallArgs`). Nothing about it was specific to arguments: it already
// answered "does a value of type V fit a slot of type T", which is the same
// question local declarations, assignments, returns, trailing expressions and
// parameter defaults were asking and silently not checking.

#include "Volt/MiddleEnd/TypeSystem/TypeCompat.hpp"

#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace Volt::MiddleEnd::TypeSystem
{

namespace
{

    // The layout of a nominal, when it has one and it is a scalar. Nothing here
    // looks at a type *name*: a `Primitive` is an opaque interned spelling plus a
    // bit width, which is the whole vocabulary `rules/zero-hardcode.md` grants the
    // C++ side.
    [[nodiscard]] std::optional<Primitive> PrimitiveOf ( const TypeStore &Types, NominalId Base )
    {
        if ( not Base.IsValid() )
        {
            return std::nullopt;
        }
        const LayoutId Layout = Types.Type( Base ).Layout;
        if ( not Layout.IsValid() )
        {
            return std::nullopt;
        }
        const LayoutNode &Node = Types.Get( Layout );
        if ( KindOf( Node ) != LayoutKind::Primitive )
        {
            return std::nullopt;
        }
        return std::get<Primitive>( Node );
    }

    // Which family of machine value a spelling denotes. Exactly the classification
    // that already selects `add` vs `fadd` at codegen time ("the spelling selects
    // the instruction"), reused rather than duplicated: `"i32"`/`"u8"` are integers,
    // `"f64"` is floating point, and anything else (`"ptr"`, `"i1"`... ) is its own
    // family, matched by the full spelling.
    [[nodiscard]] std::string_view ScalarFamily ( std::string_view Spelling )
    {
        if ( Spelling.starts_with( 'i' ) or Spelling.starts_with( 'u' ) )
        {
            // `i1` is a truth value, not a one-bit integer: keeping it in its own
            // family is what stops a boolean from silently widening into a number.
            return Spelling == "i1" ? Spelling : "int";
        }
        if ( Spelling.starts_with( 'f' ) )
        {
            return "float";
        }
        return Spelling;
    }

    // **Implicit widening between scalars of the same family.**
    //
    // Not in the original plan, and not a loosening to silence an error — it is the
    // answer to a gap the plan did not know about, found by turning the check on
    // (§C.3). `mixin Hashable` contracts `hash -> UInt64`, because `Hash#[]=`
    // computes `key.hash % @entries.capacity` against a UInt64 capacity. Every
    // narrower width must therefore answer a UInt64, and `Int8#hash` can only be
    // written `self`. Volt has **no conversion between integer widths** — no cast,
    // no `to_u64`, and `@[Intrinsic]` is recognised nowhere in the compiler — so
    // without this rule `hash` is literally unwritable for 5 of the 10 primitive
    // widths, and `volt check source/Lib/` can never reach zero.
    //
    // The rule is deliberately the narrow one:
    //
    //   * both nominals **non-generic** — a generic's identity is not exhausted by
    //     its layout. `Pointer<T>` is `@[Primitive( "ptr", 64 )]` for *every* `T`,
    //     so without this guard `Pointer<Int32>` would be assignable to
    //     `Pointer<String>`. That is the one case this must never accept.
    //   * **same family** — no float→int truncation, no bool→int.
    //   * **never narrowing** — `Target.Bits >= Value.Bits`. `UInt64` into `Int32`
    //     stays an error.
    //
    // Sign is deliberately not part of the identity: `i8 -> u64` is accepted, which
    // is the `hash` case, and is a reinterpretation the backend emits as a plain
    // zext/sext. Equal widths of the same family are therefore interchangeable,
    // which is also what makes `Char` (`"u8"`) storable into a `Pointer<UInt8>`
    // buffer — the other place the missing-conversion gap shows up.
    [[nodiscard]] bool IsWideningScalar ( const TypeCheckerContext &Context, SemaTypeId Target, SemaTypeId Value )
    {
        const SemaType &TargetVal = Context.Ctx.Values.Get( Target );
        const SemaType &ValueVal  = Context.Ctx.Values.Get( Value );
        if ( not TargetVal.Args.IsEmpty() or not ValueVal.Args.IsEmpty() )
        {
            return false;
        }

        const std::optional<Primitive> TargetPrim = PrimitiveOf( Context.Ctx.Types, TargetVal.Base );
        const std::optional<Primitive> ValuePrim  = PrimitiveOf( Context.Ctx.Types, ValueVal.Base );
        if ( not TargetPrim.has_value() or not ValuePrim.has_value() )
        {
            return false;
        }

        const std::string_view TargetSpelling = Context.Ctx.Types.Text( TargetPrim->Spelling );
        const std::string_view ValueSpelling  = Context.Ctx.Types.Text( ValuePrim->Spelling );
        if ( ScalarFamily( TargetSpelling ) != ScalarFamily( ValueSpelling ) )
        {
            return false;
        }
        return TargetPrim->Bits >= ValuePrim->Bits;
    }

    // `null` fits any slot laid out the way `Nil` itself is — in practice every
    // `Pointer<T>`, whose layout is the same pointer-width scalar.
    //
    // Nil is identified by the **node kind it claims** (`@[Literal( NilLiteral )]`),
    // resolved through LookupNodeKind exactly as `IntLiteral` is. The compiler
    // never learns the type is spelled "Nil", so the zero-hardcode guard holds.
    //
    // This needs its own rule rather than falling out of IsWideningScalar because
    // `Pointer<T>` is generic, and that predicate deliberately refuses every
    // generic — otherwise `Pointer<Int32>` would be assignable to `Pointer<String>`.
    // Nil is the one value for which the generic argument genuinely does not
    // matter, since it carries no pointee at all.
    [[nodiscard]] bool IsNilInto ( const TypeCheckerContext &Context, SemaTypeId Target, SemaTypeId Value )
    {
        const std::optional<NominalId> NilBase = Context.Ctx.Types.LookupNodeKind( "NilLiteral" );
        if ( not NilBase.has_value() or Context.Ctx.Values.Get( Value ).Base != *NilBase )
        {
            return false;
        }

        const std::optional<Primitive> NilPrim    = PrimitiveOf( Context.Ctx.Types, *NilBase );
        const std::optional<Primitive> TargetPrim = PrimitiveOf( Context.Ctx.Types, Context.Ctx.Values.Get( Target ).Base );
        if ( not NilPrim.has_value() or not TargetPrim.has_value() )
        {
            return false;
        }
        return Context.Ctx.Types.Text( NilPrim->Spelling ) == Context.Ctx.Types.Text( TargetPrim->Spelling ) and
               NilPrim->Bits == TargetPrim->Bits;
    }

    // A `Circle` stored in a slot declared `Shape`. Subtyping proper, and the
    // one rule that makes an inheritance hierarchy usable: without it a base
    // -typed local, parameter or `Array<Shape>` element can only ever hold the
    // base itself, which is what every `assert!( total_area( shapes ) )` shape
    // of program needs.
    //
    // `ConformsTo` rather than `IsSubclassOf` so an `include`d mixin counts:
    // the members a value has and the slots it fits are the same question.
    //
    // Deliberately refuses a target that carries generic arguments. Reaching
    // `Box<Int32>` from `class IntBox < Box<Int32>` means instantiating each
    // `Super` link down the chain against the arguments below it, and nothing
    // here does that — so a wrong `true` would silently accept `Box<String>`
    // too. Same-base generics never come through here: they are settled by the
    // argument-wise comparison in IsAssignable itself.
    [[nodiscard]] bool IsSubtypeInto ( const TypeCheckerContext &Context, SemaTypeId Target, SemaTypeId Value )
    {
        const SemaType &TargetVal = Context.Ctx.Values.Get( Target );
        if ( not TargetVal.Args.IsEmpty() )
        {
            return false;
        }
        const NominalId TargetBase = TargetVal.Base;
        if ( not TargetBase.IsValid() or Context.Ctx.Types.HasAbstractMembers( TargetBase ) )
        {
            return false;
        }
        return ConformsTo( Context.Ctx.Types, Context.Ctx.Values.Get( Value ).Base, TargetBase );
    }

    [[nodiscard]] bool IsDynamicInto ( const TypeCheckerContext &Context, SemaTypeId Target, SemaTypeId Value )
    {
        if ( not Context.Ctx.Values.Has( Target ) or not Context.Ctx.Values.Has( Value ) )
        {
            return false;
        }
        const SemaType &TargetVal = Context.Ctx.Values.Get( Target );
        if ( not TargetVal.Base.IsValid() or not Context.Ctx.Types.IsNodeKind( "DynamicType", TargetVal.Base ) )
        {
            return false;
        }
        if ( TargetVal.Args.IsEmpty() or not TargetVal.Args[0].IsValid() )
        {
            return false;
        }
        const SemaType &ValueVal  = Context.Ctx.Values.Get( Value );
        const NominalId TraitBase = Context.Ctx.Values.Get( TargetVal.Args[0] ).Base;

        if ( Context.Ctx.Types.IsNodeKind( "DynamicType", ValueVal.Base ) )
        {
            if ( not ValueVal.Args.IsEmpty() and ValueVal.Args[0].IsValid() )
            {
                const NominalId ValueTraitBase = Context.Ctx.Values.Get( ValueVal.Args[0] ).Base;
                return ConformsTo( Context.Ctx.Types, ValueTraitBase, TraitBase );
            }
            return false;
        }

        return ConformsTo( Context.Ctx.Types, ValueVal.Base, TraitBase );
    }

} // namespace

bool IsAssignable ( const TypeCheckerContext &Context, SemaTypeId Target, SemaTypeId Value )
{
    if ( Target.Value == Value.Value )
    {
        return true;
    }
    if ( not Context.Ctx.Values.Has( Value ) or not Context.Ctx.Values.Has( Target ) )
    {
        return false;
    }

    const SemaType &ValueVal  = Context.Ctx.Values.Get( Value );
    const SemaType &TargetVal = Context.Ctx.Values.Get( Target );
    if ( ValueVal.Base != TargetVal.Base or ValueVal.Args.Size() != TargetVal.Args.Size() )
    {
        return IsWideningScalar( Context, Target, Value ) or IsNilInto( Context, Target, Value ) or
               IsDynamicInto( Context, Target, Value ) or IsSubtypeInto( Context, Target, Value );
    }

    // A slot the declaration side left open (invalid — an un-instantiated
    // generic, or a written argument that named no type at all) matches
    // whatever the value actually supplies there, the same way an
    // un-instantiated method generic does. This is what lets `Pointer<Void>`
    // stand in for "any pointer" in the external `memcpy` / `memcmp`
    // signatures without the compiler ever comparing a type name.
    for ( std::size_t Index = 0; Index < TargetVal.Args.Size(); ++Index )
    {
        if ( not TargetVal.Args[Index].IsValid() )
        {
            continue;
        }
        if ( TargetVal.Args[Index].Value != ValueVal.Args[Index].Value )
        {
            return false;
        }
    }
    return true;
}

void CheckAssignable ( TypeCheckerContext &Context, Frontend::ExprId ValueExpr, SemaTypeId Target, EAssignSite Site )
{
    // An unresolved slot is not a mismatch. `-> Void` in particular resolves to
    // an invalid SemaTypeId, which is exactly why a Void method's trailing
    // expression is not checked against anything — and why this needs no test
    // against the name "Void", which would be a hardcoded Volt type.
    if ( not ValueExpr.IsValid() or not Target.IsValid() )
    {
        return;
    }

    const SemaTypeId Value = Context.Ctx.Values.ExprType( ValueExpr );
    if ( not Value.IsValid() or Context.IsNoReturn( Value ) )
    {
        return;
    }
    if ( IsAssignable( Context, Target, Value ) )
    {
        return;
    }

    const std::string From = Context.NameOfValue( Value );
    const std::string To   = Context.NameOfValue( Target );
    const bool bReturning  = Site == EAssignSite::Return or Site == EAssignSite::Trailing;

    Context.Report( Frontend::LocOf( Context.Ctx.Ast.Expr( ValueExpr ) ),
                    bReturning ? "cannot return " + From + " from a method declared " + To
                               : "cannot assign " + From + " to " + To );
}

} // namespace Volt::MiddleEnd::TypeSystem
