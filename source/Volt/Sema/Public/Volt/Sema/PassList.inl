// PassList.inl — the single ordered manifest of semantic passes.
//
// Re-included with a different definition of VOLT_PASS to generate the pass
// function declarations and the sorted registry. Reliability over cleverness:
// a manifest sidesteps the static-init-order and dead-strip footguns that bite
// inline self-registration inside a static/shared lib.
//
//         Pass          Order
#ifndef VOLT_PASS
    #define VOLT_PASS( Name, Order )
#endif

VOLT_PASS( ScopeResolver, 10 )
VOLT_PASS( JsxLowering, 20 )
VOLT_PASS( TypeChecker, 30 )

#undef VOLT_PASS
