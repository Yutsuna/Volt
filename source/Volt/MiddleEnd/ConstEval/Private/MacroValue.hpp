#pragma once

// MacroValue.hpp — the compiler-internal value model of compile-time
// evaluation, and the bridge between it and the AST.
//
// A macro value is a *compiler* value: truth, an integer, text, a sequence.
// None of it is a Volt type — the bridge back into the AST is a literal node,
// which the existing `@[Literal( ... )]` binding types with no help from here
// (rules/zero-hardcode.md). Member operations come from the MacroOps.inl
// manifest and nowhere else.

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Volt
{

namespace MiddleEnd::ConstEval
{

    struct MacroValue
    {

        std::variant<std::monostate, bool, std::int64_t, std::string, std::vector<MacroValue>> Data;
    };

    [[nodiscard]] bool Truthy ( const MacroValue &Value );

    /// The text a value splices as. Sequences render bracketed, which is only
    /// ever a diagnostic aid — a spliced sequence is a mistake worth seeing.
    [[nodiscard]] std::string Stringify ( const MacroValue &Value );

    /// The value a literal node carries, or nullopt when the node is not a
    /// compile-time literal. This is what makes the fold sweep compositional:
    /// a folded inner node is indistinguishable from one the source wrote.
    [[nodiscard]] std::optional<MacroValue> ValueOfLiteral ( const Frontend::AstContext &Ast, Frontend::ExprId Id );

    /// The literal node a value folds to: text becomes a StringLiteral, a
    /// sequence an ArrayLit of them, truth a BoolLiteral. The nodes are the
    /// ordinary ones, so nothing downstream learns that a macro produced them.
    [[nodiscard]] Frontend::ExprNode
    LiteralOfValue ( Frontend::AstContext &Ast, const MacroValue &Value, ::Volt::Core::SourceRange Loc );

    /// Whether Spelling names an operation of the manifest.
    [[nodiscard]] bool IsMacroOp ( std::string_view Spelling );

    /// Apply a manifest operation. nullopt when the spelling is not one of
    /// them; a diagnostic and a nil value when it is but the receiver or the
    /// arguments do not fit it.
    [[nodiscard]] std::optional<MacroValue> ApplyMacroOp ( std::string_view Spelling,
                                                           const MacroValue &Receiver,
                                                           std::span<const MacroValue> Args,
                                                           ::Volt::Core::DiagEngine::Bag &Diags,
                                                           ::Volt::Core::SourceRange Loc );

} // namespace MiddleEnd::ConstEval

} // namespace Volt
