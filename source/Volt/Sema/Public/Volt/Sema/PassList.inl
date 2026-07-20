// PassList.inl — the single ordered manifest of semantic passes.
//
// Re-included with a different definition of VOLT_PASS to generate the pass
// function declarations and the sorted registry. Reliability over cleverness:
// a manifest sidesteps the static-init-order and dead-strip footguns that bite
// inline self-registration inside a static/shared lib.
//
// Kind (EPassKind) marks what a pass does to the AST, so tools can run a
// subset: `volt parse --lowered` runs only the Lowering passes, and
// `check --type` will build on the same axis.
//
//         Pass           Order  Kind
#ifndef VOLT_PASS
    #define VOLT_PASS( Name, Order, Kind )
#endif

VOLT_PASS( ScopeResolver, 10, Analysis )
VOLT_PASS( JsxLowering, 20, Lowering )
VOLT_PASS( CaseLowering, 22, Lowering )
VOLT_PASS( TypeChecker, 30, Analysis )

#undef VOLT_PASS
