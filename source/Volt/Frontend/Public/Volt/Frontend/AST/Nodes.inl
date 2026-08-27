// Nodes.inl — the single manifest that drives every AST category.
//
// Re-included with different definitions of VOLT_EXPR / VOLT_STMT / VOLT_DECL /
// VOLT_TYPE to generate the per-category Kind enum and the std::variant.
// Field names come from P2996 reflection. Adding a node = one line here +
// its plain struct.

#ifndef VOLT_EXPR
    #define VOLT_EXPR( Name )
#endif
// A *sugar* expression: one the Lowering passes must have removed from the
// arena before TypeChecker runs. It is an ordinary VOLT_EXPR for every
// consumer that does not care — the enum and the variant still see all 36
// nodes — so marking one costs a single line here. Only a consumer that
// defines VOLT_EXPR_SUGAR itself tells the two apart, which is how the
// AstInvariant pass builds its membership set without a switch.
//
// Adding a sugar node = one line. Forgetting to lower it = a build error,
// not a discovery made three months later inside a backend.
#ifndef VOLT_EXPR_SUGAR
    #define VOLT_EXPR_SUGAR( Name ) VOLT_EXPR( Name )
#endif
#ifndef VOLT_STMT
    #define VOLT_STMT( Name )
#endif
#ifndef VOLT_DECL
    #define VOLT_DECL( Name )
#endif
#ifndef VOLT_TYPE
    #define VOLT_TYPE( Name )
#endif

// --- Expressions -----------------------------------------------------------
VOLT_EXPR( IntLiteral )
VOLT_EXPR( FloatLiteral )
VOLT_EXPR( StringLiteral )
VOLT_EXPR( CharLiteral )
VOLT_EXPR( BoolLiteral )
VOLT_EXPR( NilLiteral )
VOLT_EXPR( SymbolLiteral )
VOLT_EXPR_SUGAR( ArrayLit )
VOLT_EXPR_SUGAR( HashLit )
VOLT_EXPR( Identifier )
VOLT_EXPR( InstanceVar )
VOLT_EXPR( SelfExpr )
VOLT_EXPR( SuperExpr )
VOLT_EXPR( Binary )
VOLT_EXPR( Unary )
VOLT_EXPR_SUGAR( Postfix )
VOLT_EXPR( Ternary )
VOLT_EXPR( Assign )
VOLT_EXPR( Call )
VOLT_EXPR_SUGAR( Block )
VOLT_EXPR_SUGAR( Index )
VOLT_EXPR( Member )
VOLT_EXPR( GenericInst )
VOLT_EXPR( SizeOf )
VOLT_EXPR( TypeTrait )
VOLT_EXPR_SUGAR( TypeOfExpr )
VOLT_EXPR( FuncAddr )
VOLT_EXPR( DynamicUpcast )
VOLT_EXPR( TypedExpr )
VOLT_EXPR( Deref )
VOLT_EXPR_SUGAR( Interp )
VOLT_EXPR_SUGAR( CommandLit )
VOLT_EXPR_SUGAR( IvarInterp )
VOLT_EXPR_SUGAR( JsxElement )
VOLT_EXPR_SUGAR( JsxFragment )
VOLT_EXPR_SUGAR( JsxText )
VOLT_EXPR( CaseExpr )
VOLT_EXPR_SUGAR( DotCall )
VOLT_EXPR_SUGAR( Pipeline )
VOLT_EXPR_SUGAR( Lambda )
VOLT_EXPR_SUGAR( Section )
VOLT_EXPR_SUGAR( Composition )
VOLT_EXPR( RaiseExpr )
VOLT_EXPR( BeginExpr )
VOLT_EXPR( If )

// --- Statements ------------------------------------------------------------
VOLT_STMT( ExprStmt )
VOLT_STMT( While )
VOLT_STMT( Return )
VOLT_STMT( Break )
VOLT_STMT( Next )
VOLT_STMT( LocalDecl )
VOLT_STMT( WhenClause )
VOLT_STMT( RescueClause )

// --- Declarations ----------------------------------------------------------
VOLT_DECL( Module )
VOLT_DECL( Class )
VOLT_DECL( Struct )
VOLT_DECL( Mixin )
VOLT_DECL( Method )
VOLT_DECL( Field )
VOLT_DECL( ExternalVar )
VOLT_DECL( Include )
VOLT_DECL( Component )
VOLT_DECL( Circuit )
VOLT_DECL( Annotation )
VOLT_DECL( MacroDef )
VOLT_DECL( MacroBlock )
VOLT_DECL( Enum )
VOLT_DECL( EnumCase )

// --- Types -----------------------------------------------------------------
VOLT_TYPE( TypeRef )
VOLT_TYPE( PointerType )
VOLT_TYPE( NilableType )
VOLT_TYPE( FixedArrayType )
VOLT_TYPE( FuncType )
VOLT_TYPE( TypeOfType )
VOLT_TYPE( DynamicType )

#undef VOLT_EXPR
#undef VOLT_EXPR_SUGAR
#undef VOLT_STMT
#undef VOLT_DECL
#undef VOLT_TYPE
