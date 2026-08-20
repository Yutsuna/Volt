#pragma once

// MacroEval.hpp — the compile-time evaluator behind `macro def` / `macro do`.
//
// A macro body is not a template: it is an ordinary parsed Volt program that
// runs at compile time and whose *output* is the body of a generated method.
// Nothing here lexes or parses anything, so nothing here can leave a half-typed
// node behind — what the evaluator does not execute, it emits as real AST in
// the target type's own arena.
//
// Staging is decided by the data, not by syntax (.agents/middleend/macros.md,
// rules R1-R8):
//
//   * a *compile-time source* is introspection of the target type (`self`,
//     `self.fields`), a command literal (`` `uname` ``), a magic constant
//     (`__DIR__`), or a loop variable bound to one of those;
//   * a binding whose initialiser flows from a source is compile-time and
//     disappears — `json = "{"` is a plain literal, so it stays a runtime local;
//   * a value the evaluator can *compute* is not necessarily one it may
//     *substitute*: `content.size` folds into the emitted code because `size`
//     is in MacroOps.inl, while `content.size > 0` is computed (so a `if` over
//     it can be decided at compile time) yet emitted as written, which is what
//     leaves `assert!( 1024 > 0 )` in the generated method;
//   * `if` / `case` over a computable condition visits only the winning branch,
//     and `for x in <compile-time sequence>` is unrolled once per element;
//   * everything else is emitted, with its foldable parts substituted in place.
//
// The one node whose emission changes kind is `@#{ ... }` (IvarInterp): once
// its name has a value it becomes an ordinary InstanceVar, whose interned
// lexeme keeps the `@` (MemberResolver.cpp reads it that way).

#include "MacroValue.hpp"

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/MiddleEnd/ConstEval/MagicConstants.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace Volt
{

namespace MiddleEnd::ConstEval
{

    /// Everything one macro evaluation reads. Source and Target are frequently
    /// the *same* AstContext (a `macro def` written in the class it generates
    /// for) and are deliberately allowed to be: the evaluator copies every node
    /// out of Source before it adds anything to Target, so a growing arena can
    /// never invalidate what it is reading (rules/ast-rewrite.md).
    struct MacroEnv
    {

        const Frontend::AstContext &Source; //< where the macro body lives (the mixin, or the type)
        Frontend::AstContext &Target;       //< where the generated nodes are built (the target type)
        const TypeSystem::TypeStore &Store;
        /// Every unit's AST, indexed by the ordinal Member::Unit / NominalType::Unit
        /// carry — a target type's field may be declared in a unit that is
        /// neither Source nor Target. A null entry is a cache-served stdlib
        /// unit, skipped exactly as the rest of the seam skips it.
        std::span<Frontend::AstContext *const> Units;
        /// The type `self` introspects. Invalid inside a `macro do`, where
        /// there is no target type and `self` is simply not a source.
        TypeSystem::NominalId SelfType;
        MagicSite Site; //< `__DIR__` & co, from the one manifest
        ::Volt::Core::DiagEngine::Bag &Diags;
        std::string WorkDir; //< where a command runs: the compiled file's own directory
        std::uint32_t Depth = 0;
    };

    /// Run Body at compile time, appending to Out the statements it emits —
    /// built in Env.Target, ready to become a method's body or to be discarded
    /// (a `macro do` emits nothing by construction).
    ///
    /// bTailValue marks a body whose last statement is the generated method's
    /// result: a compile-time value there is emitted as its literal instead of
    /// vanishing, since the method has to return something.
    void EvalMacroBody ( MacroEnv &Env, const Frontend::StmtList &Body, Frontend::StmtList &Out, bool bTailValue );

} // namespace MiddleEnd::ConstEval

} // namespace Volt
