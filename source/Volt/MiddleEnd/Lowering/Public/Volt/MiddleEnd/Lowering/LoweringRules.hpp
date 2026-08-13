#pragma once

// LoweringRules.hpp — the declarative rewrite engine the syntactic lowerings
// are written against.
//
// A lowering pass is always the same program: sweep the Expr arena by index,
// find the nodes of one kind, replace each with the core-AST shape it desugars
// to. Every pass in this module used to spell that sweep out again — its own
// `class XRewriter`, its own `for` over `ExprCount()`, its own `KindOf` test,
// its own copy-out/write-back. The sweep is the part that is easy to get
// subtly wrong (rules/ast-rewrite.md: never hold a reference across `Add()`,
// never let the loop see nodes it just appended), and it was duplicated once
// per pass.
//
// Here it is written once. What a pass supplies is a *rule*: which node kind
// it claims, whether a given node is really its business, and what that node
// becomes. Everything else — the bound, the ordering, the write-back, the
// counter — belongs to `RuleRegistry::Run`.
//
// The registry dispatches on node kind, so a rule's `Match` is only ever
// called on nodes of the kind it claimed: adding a rule costs nothing to the
// passes that do not claim that kind, which is what keeps "one more rule" from
// meaning "one more test on every node in the file".

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "VoltMiddleEndLowering_export.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <string_view>
#include <variant>
#include <vector>

namespace Volt::MiddleEnd::Lowering
{

// Everything a rule is allowed to touch. Deliberately smaller than
// `Core::PassContext`: a syntactic rewrite runs before any type exists, so a
// rule that could reach `Types`/`Values` would be a rule that could depend on
// them — which is the property that lets these run at order 8-26 at all.
struct RewriteContext
{

    Frontend::AstContext &Ast;
    Volt::Core::DiagEngine::Bag &Diags;
};

// A rewrite rule. Three members, none of them a loop:
//
//   Kind()   the ExprKind this rule claims. The registry buckets on it, so
//            `Match` is never called on a node of any other kind.
//   Match()  is *this* node this rule's business? Called only for the claimed
//            kind, so it answers the residual question — "is the RHS already a
//            Call?" — never "is this a Pipeline?". A rule that claims every
//            node of its kind returns true and costs nothing.
//   Apply()  the node it becomes. Returned by value and written back by the
//            registry, which is what keeps `rules/ast-rewrite.md`'s copy-out /
//            write-back discipline out of every individual rule.
//
// Stateless by construction: the registry default-constructs one per call, so
// a rule cannot accumulate anything across the nodes it visits.
template <typename T>
concept CRewriteRule = requires( T Rule, RewriteContext &Ctx, Frontend::ExprId Id ) {
    { T::Kind() } -> std::same_as<Frontend::ExprKind>;
    { Rule.Match( Ctx, Id ) } -> std::same_as<bool>;
    { Rule.Apply( Ctx, Id ) } -> std::same_as<Frontend::ExprNode>;
};

// One registered rule, type-erased to a pair of function pointers so rules of
// unrelated types can share a bucket. Stateless rules mean no allocation and
// no indirection beyond the call itself.
struct RewriteRule
{

    std::string_view Name;
    int Priority                                                        = 0;
    bool ( *Match )( RewriteContext &, Frontend::ExprId )               = nullptr;
    Frontend::ExprNode ( *Apply )( RewriteContext &, Frontend::ExprId ) = nullptr;
};

// The rules of one pass, bucketed by the kind each claims.
//
// Priority orders rules *within* a bucket, highest first, and the first whose
// `Match` accepts wins — so a specific rule and a catch-all sibling replace
// what would otherwise be an `if/else` inside one rule's `Apply`. That is the
// point: `a |> f( x )` and `a |> f` are two rules, not two branches.
class VOLT_MIDDLEEND_LOWERING_EXPORT RuleRegistry
{

public:

    // Registers `T` under `Name`. Higher `Priority` is consulted first within
    // the kind `T` claims; equal priorities keep registration order.
    template <CRewriteRule T> RuleRegistry &Add ( std::string_view Name, int Priority = 0 )
    {
        Insert( RewriteRule{ .Name     = Name,
                             .Priority = Priority,
                             .Match    = [] ( RewriteContext &Ctx, Frontend::ExprId Id ) { return T{}.Match( Ctx, Id ); },
                             .Apply    = [] ( RewriteContext &Ctx, Frontend::ExprId Id ) { return T{}.Apply( Ctx, Id ); } },
                T::Kind() );
        return *this;
    }

    // One bounded index sweep of the whole Expr arena; returns how many nodes
    // were rewritten.
    //
    // Bounded at the count taken *before* the sweep, so nodes a rule appends
    // are never themselves visited — a rule's output is core AST by
    // definition, and re-examining it is how a rewrite engine loops forever.
    // Sub-expressions parse first and so hold smaller indices, which is what
    // makes ascending order lower the innermost node of a nest first
    // (`5 |> add |> double`), exactly as the hand-written sweeps did.
    //
    // At most one rule fires per node per sweep: the first match wins and the
    // node is done. A rewrite that needs a second rule to run over its own
    // output wants a second pass, not a re-entrant sweep.
    [[nodiscard]] std::size_t Run ( RewriteContext &Context ) const;

private:

    void Insert ( RewriteRule Rule, Frontend::ExprKind Kind );

    // Indexed by ExprKind. `std::variant_size_v` counts the alternatives of
    // ExprNode — monostate plus one per VOLT_EXPR row — which is exactly the
    // number of ExprKind values, so a new node kind widens this with no edit.
    static constexpr std::size_t KindCount = std::variant_size_v<Frontend::ExprNode>;

    std::array<std::vector<RewriteRule>, KindCount> Buckets{};
};

} // namespace Volt::MiddleEnd::Lowering
