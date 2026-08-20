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

    struct MacroValue;

    /// One field of the target type, as the *source wrote it*: the declared
    /// name and the declared type's spelling. Neither is resolved — the seam
    /// runs before signatures do — and neither needs to be: a macro emits code
    /// in the user's own vocabulary, so handing back the spelling is both what
    /// generation wants and what keeps a Volt type name out of the C++
    /// (rules/zero-hardcode.md).
    struct MacroFieldDesc
    {

        std::string Name;
        std::string Type;
    };

    /// The target type itself — what `self` is worth inside a macro body. The
    /// fields are captured when `self` is evaluated rather than looked up
    /// later, so the value model never has to reach back into the TypeStore.
    struct MacroTypeDesc
    {

        std::string Name;
        std::vector<MacroFieldDesc> Fields;
    };

    struct MacroValue
    {

        std::variant<std::monostate, bool, std::int64_t, std::string, std::vector<MacroValue>, MacroFieldDesc, MacroTypeDesc>
            Data;
    };

    [[nodiscard]] bool Truthy ( const MacroValue &Value );

    /// The text a value splices as. Sequences render bracketed, which is only
    /// ever a diagnostic aid — a spliced sequence is a mistake worth seeing.
    [[nodiscard]] std::string Stringify ( const MacroValue &Value );

    /// The value a literal node carries, or nullopt when the node is not a
    /// compile-time literal. This is what makes the fold sweep compositional:
    /// a folded inner node is indistinguishable from one the source wrote.
    [[nodiscard]] std::optional<MacroValue> ValueOfLiteral ( const Frontend::AstContext &Ast, Frontend::ExprId Id );

    /// The same question asked of a node that is not in an arena — the literal
    /// `ExpandMagic` hands back, which the evaluator needs as a *value* rather
    /// than as a node to splice.
    [[nodiscard]] std::optional<MacroValue> ValueOfLiteralNode ( const Frontend::AstContext &Ast,
                                                                 const Frontend::ExprNode &Literal );

    /// The literal node a value folds to: text becomes a StringLiteral, a
    /// sequence an ArrayLit of them, truth a BoolLiteral. The nodes are the
    /// ordinary ones, so nothing downstream learns that a macro produced them.
    [[nodiscard]] Frontend::ExprNode
    LiteralOfValue ( Frontend::AstContext &Ast, const MacroValue &Value, ::Volt::Core::SourceRange Loc );

    /// Whether Spelling names an operation of the manifest.
    [[nodiscard]] bool IsMacroOp ( std::string_view Spelling );

    /// Run a host command on behalf of a macro and hand back its stdout. Both
    /// callers — the order-15 fold sweep and the macro evaluator — need the
    /// identical policy (a bounded timeout, a capped output, a non-zero exit is
    /// a compile error carrying the command's own stderr), so it is written
    /// once here rather than agreed on twice. The captured output comes back
    /// even on failure, so evaluation carries on and reports everything that is
    /// wrong in one build instead of one problem per run.
    [[nodiscard]] MacroValue RunMacroCommand ( const std::string &Command,
                                               std::string_view WorkDir,
                                               ::Volt::Core::DiagEngine::Bag &Diags,
                                               ::Volt::Core::SourceRange Loc );

    /// Apply a manifest operation. nullopt when the operation does not apply —
    /// either the spelling is not one of them, or it is but this receiver is
    /// not a kind it operates on at all, which is an ordinary runtime member
    /// access (`"text".fields`) and not a mistake. A diagnostic and a nil value
    /// when the operation *does* claim the receiver but the operands are wrong.
    [[nodiscard]] std::optional<MacroValue> ApplyMacroOp ( std::string_view Spelling,
                                                           const MacroValue &Receiver,
                                                           std::span<const MacroValue> Args,
                                                           ::Volt::Core::DiagEngine::Bag &Diags,
                                                           ::Volt::Core::SourceRange Loc );

} // namespace MiddleEnd::ConstEval

} // namespace Volt
