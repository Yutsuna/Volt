// Nodes.inl — the single manifest that drives every AST category.
//
// Re-included with different definitions of VOLT_EXPR / VOLT_STMT / VOLT_DECL /
// VOLT_TYPE to generate the per-category Kind enum and the std::variant.
// Field names come from P2996 reflection. Adding a node = one line here +
// its plain struct.

#ifndef VOLT_EXPR
    #define VOLT_EXPR( Name )
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
VOLT_EXPR( ArrayLit )
VOLT_EXPR( HashLit )
VOLT_EXPR( Identifier )
VOLT_EXPR( InstanceVar )
VOLT_EXPR( SelfExpr )
VOLT_EXPR( Binary )
VOLT_EXPR( Unary )
VOLT_EXPR( Ternary )
VOLT_EXPR( Assign )
VOLT_EXPR( Call )
VOLT_EXPR( Block )
VOLT_EXPR( Index )
VOLT_EXPR( Member )
VOLT_EXPR( GenericInst )
VOLT_EXPR( SizeOf )
VOLT_EXPR( Deref )
VOLT_EXPR( Interp )
VOLT_EXPR( JsxElement )
VOLT_EXPR( JsxFragment )
VOLT_EXPR( JsxText )

// --- Statements ------------------------------------------------------------
VOLT_STMT( ExprStmt )
VOLT_STMT( If )
VOLT_STMT( While )
VOLT_STMT( Return )
VOLT_STMT( LocalDecl )

// --- Declarations ----------------------------------------------------------
VOLT_DECL( Module )
VOLT_DECL( Class )
VOLT_DECL( Struct )
VOLT_DECL( Mixin )
VOLT_DECL( Method )
VOLT_DECL( Field )
VOLT_DECL( Include )
VOLT_DECL( Component )
VOLT_DECL( Circuit )
VOLT_DECL( Annotation )

// --- Types -----------------------------------------------------------------
VOLT_TYPE( TypeRef )
VOLT_TYPE( PointerType )
VOLT_TYPE( NilableType )
VOLT_TYPE( FixedArrayType )
VOLT_TYPE( FuncType )

#undef VOLT_EXPR
#undef VOLT_STMT
#undef VOLT_DECL
#undef VOLT_TYPE
