// PassList.inl — the single ordered manifest of semantic passes.
//
// Re-included with a different definition of VOLT_PASS to generate the pass
// function declarations and the sorted registry. Reliability over cleverness:
// a manifest sidesteps the static-init-order and dead-strip footguns that bite
// inline self-registration inside a static/shared lib.
//
// Type *binding* is deliberately absent here: it is cross-unit (a user file's
// `10` resolves to the Int32 declared in source/Lib/), so it cannot be a
// per-file parallel pass. The Driver calls Sema::BindUnitTypes serially in the
// same seam that publishes interfaces — see Layout/TypeBinder.hpp.
//
// Kind (EPassKind) marks what a pass does to the AST, so tools can run a
// subset: `volt parse --lowered` runs only the Lowering passes, and
// `check --type` will build on the same axis.
//
//         Pass           Order  Kind
#ifndef VOLT_PASS
    #define VOLT_PASS( Name, Order, Kind )
#endif

VOLT_PASS( FunctionalLowering, 8, Lowering )
VOLT_PASS( PipelineLowering, 9, Lowering )
VOLT_PASS( ScopeResolver, 10, Analysis )
// Enum-case ordinal materialization used to live here (order 12) but moved
// to Frontend::MaterializeEnumOrdinals (EnumSynthesis.cpp), running from
// Driver::ParseOne before Sema::BindUnitTypes: it was already purely
// syntactic (no type information needed), and TypeBinder's Phase A now
// caches each case's ordinal onto its Member (Member::EnumOrdinal), which
// requires the value to already be materialized by the time Phase A runs —
// earlier than any Sema pass in this list ever executes.

VOLT_PASS( MacroExpansion, 15, Lowering )
// After MacroExpansion on purpose: macro-generated text gets its constants
// inlined too, and the re-parsed nodes carry the invocation's SourceRange, so
// `__LINE__` in a macro body reports the caller's line.
VOLT_PASS( MagicExpansion, 16, Lowering )
VOLT_PASS( JsxLowering, 20, Lowering )
VOLT_PASS( CaseLowering, 22, Lowering )
// Right after CaseLowering, never before: a `when .even?` pattern is a DotCall
// that belongs to the scrutinee, not to `self`. What survives order 22 is a
// `.method` in statement position, i.e. a genuine implicit-self call.
VOLT_PASS( DotCallLowering, 23, Lowering )
// Before IndexLowering: it turns `obj[ i ] += v` into `obj[ i ] = obj[ i ] + v`,
// so the store lowering below sees one shape of assignment and nothing else.
VOLT_PASS( AssignLowering, 24, Lowering )
VOLT_PASS( IndexLowering, 25, Lowering )
// After MacroExpansion (15), never before: a macro body is text, and the
// `#{ ... }` it contains only becomes an Interp node once that text has been
// expanded and re-parsed.
VOLT_PASS( InterpLowering, 26, Lowering )
VOLT_PASS( TypeChecker, 30, Analysis )
VOLT_PASS( UnusedChecker, 35, Analysis )
// Last, and Analysis: it creates no node, which is what lets it run after
// TypeChecker without breaking the structural invariant above. It is the
// mechanical check that the two AST contracts still hold — no residual sugar,
// no untyped value expression. See rules/core-ast.md.
VOLT_PASS( AstInvariant, 40, Analysis )

#undef VOLT_PASS
