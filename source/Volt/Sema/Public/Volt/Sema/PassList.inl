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
VOLT_PASS( EnumLowering, 12, Lowering )

VOLT_PASS( MacroExpansion, 15, Lowering )
// After MacroExpansion on purpose: macro-generated text gets its constants
// inlined too, and the re-parsed nodes carry the invocation's SourceRange, so
// `__LINE__` in a macro body reports the caller's line.
VOLT_PASS( MagicExpansion, 16, Lowering )
VOLT_PASS( JsxLowering, 20, Lowering )
VOLT_PASS( CaseLowering, 22, Lowering )
VOLT_PASS( TypeChecker, 30, Analysis )

#undef VOLT_PASS
