#pragma once

// TraitEngine.hpp — compile-time introspection: the questions a program asks
// about a value's *type*, answered here and replaced by their answer.
//
// `user.is_a? Admin` is not a call. Nothing in `source/Lib/` declares `is_a?`,
// no symbol is emitted for it, and no backend has a case for it: the middle
// end evaluates it during inference and rewrites the node into the
// `BoolLiteral` it came out as (`Analysis::FoldReceiverTrait`). That is the
// whole contract of this header — a trait that reached a backend would be a
// bug, not a slow path.
//
// Zero-hardcode (rules/zero-hardcode.md): every answer here is read off the
// `TypeStore`'s own structure — `Includes`, `Super`, `Members` — never off a
// name. The five *spellings* are compiler vocabulary, reserved in
// TokenKind.inl exactly as `trivially_destructible?` is, for the same reason:
// the compiler answers them, so a type declaring its own `is_a?` would
// otherwise be silently shadowed. No Volt type name appears in this module.

#include "Volt/Frontend/Lexer/Token.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndConstEval_export.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Volt
{

namespace MiddleEnd::ConstEval
{

    // How a trait's single argument is written. The caller needs this to know
    // what to make of the argument expression *before* evaluating: a `Type`
    // operand is resolved to a nominal, a `Name` operand is read off a symbol
    // literal, and neither is ever inferred as an ordinary value.
    enum class EOperandKind : std::uint8_t
    {

        Type, // a type name — `obj.is_a? Admin`
        Name, // a symbol    — `obj.has_field? :email`
    };

    // Everything a trait evaluation is allowed to read, resolved once per use
    // site by the caller. Widening what a trait can say is a field here plus a
    // manifest line — never a new branch.
    //
    // `Receiver` is the *nominal* behind the receiver's type, so a generic's
    // arguments are deliberately dropped: every question this file answers is
    // a per-nominal fact (`Vector<Int32>` and `Vector<String>` include the
    // same mixins and declare the same fields), which is what lets a trait in
    // a generic body fold once the receiver itself is known rather than once
    // per instantiation.
    struct TraitSite
    {

        const TypeSystem::TypeStore &Types;
        TypeSystem::NominalId Receiver;
        TypeSystem::NominalId Operand; // EOperandKind::Type
        std::string_view Name;         // EOperandKind::Name
    };

    /// The trait `Spelling` names, or nullopt if it names none. The
    /// interception seam holds an interned member name rather than a token, so
    /// this is how it gets back to the manifest row.
    [[nodiscard]] VOLT_MIDDLEEND_CONSTEVAL_EXPORT std::optional<Frontend::TokenKind> LookupTrait ( std::string_view Spelling );

    /// How `Trait`'s argument is written. Defined for every token
    /// `Frontend::IsReceiverTrait` accepts, and only those.
    [[nodiscard]] VOLT_MIDDLEEND_CONSTEVAL_EXPORT EOperandKind OperandOf ( Frontend::TokenKind Trait );

    /// The answer, for a site whose Receiver and operand the caller has
    /// already resolved. A site with an invalid `Receiver` is the caller's bug
    /// — a deferred receiver has no answer *yet* and must not reach here — so
    /// this reports `false` rather than inventing one.
    [[nodiscard]] VOLT_MIDDLEEND_CONSTEVAL_EXPORT bool EvaluateTrait ( Frontend::TokenKind Trait, const TraitSite &Site );

    /// Every spelling the manifest declares, for diagnostics. Derived from the
    /// same manifest, so it cannot drift from what EvaluateTrait knows.
    [[nodiscard]] VOLT_MIDDLEEND_CONSTEVAL_EXPORT std::span<const std::string_view> TraitNames ();

} // namespace MiddleEnd::ConstEval

} // namespace Volt
