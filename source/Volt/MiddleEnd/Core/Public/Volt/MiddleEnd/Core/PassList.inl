// PassList.inl — the single ordered manifest of semantic passes.
//
// Every pass in Volt is defined by one line in this file. Adding a pass = adding
// a line here + writing the function in the corresponding module.
// The driver, metrics, invariant checks, and ordering are all generated automatically.
//
// VOLT_PASS( Name, Order, Kind )
//
//   Name   — C++ identifier of the pass function (decl in its module).
//   Order  — Pass execution order. Low numbers run first.
//   Kind   — Analysis (reads AST, emits diags) or Lowering (rewrites AST).

//         Pass                    Order  Kind
VOLT_PASS( FunctionalLowering,       8, Lowering )
VOLT_PASS( PipelineLowering,         9, Lowering )
VOLT_PASS( ScopeResolver,           10, Analysis )
VOLT_PASS( MacroExpansion,          15, Lowering )
VOLT_PASS( MagicExpansion,          16, Lowering )
VOLT_PASS( JsxLowering,             20, Lowering )
VOLT_PASS( CaseLowering,            22, Lowering )
VOLT_PASS( DotCallLowering,         23, Lowering )
VOLT_PASS( AssignLowering,          24, Lowering )
VOLT_PASS( IndexLowering,           25, Lowering )
VOLT_PASS( InterpLowering,          26, Lowering )
VOLT_PASS( TypeChecker,             30, Analysis )
VOLT_PASS( UnusedChecker,           35, Analysis )
VOLT_PASS( AstInvariant,            40, Analysis )
