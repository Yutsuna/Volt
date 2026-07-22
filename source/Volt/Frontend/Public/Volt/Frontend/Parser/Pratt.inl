// Pratt.inl — the operator table driving ParseExpr.
//
//   VOLT_INFIX( TokenName, LeftBindingPower, RightAssociative )
//   VOLT_ASSIGN( TokenName, LeftBindingPower, RightAssociative )
//   VOLT_PREFIX( TokenName )
//
// Higher binding power binds tighter. Right-associative operators recurse at
// their own level; left-associative ones recurse one level higher. VOLT_ASSIGN
// rows are infix rows that additionally build an Assign node instead of a
// Binary one. VOLT_PREFIX rows build a Unary node at PrefixBindingPower;
// prefix/postfix forms that build *other* node shapes (deref `*p`, member,
// call, index) stay structural in ParseExpr.cpp.

#ifndef VOLT_INFIX
    #define VOLT_INFIX( Name, Lbp, RAssoc )
#endif
#ifndef VOLT_ASSIGN
    #define VOLT_ASSIGN( Name, Lbp, RAssoc )
#endif
#ifndef VOLT_PREFIX
    #define VOLT_PREFIX( Name )
#endif

// Assignment (right-associative, lowest precedence).
VOLT_ASSIGN( Assign, 10, 1 )
VOLT_ASSIGN( PlusEq, 10, 1 )
VOLT_ASSIGN( MinusEq, 10, 1 )
VOLT_ASSIGN( StarEq, 10, 1 )
VOLT_ASSIGN( SlashEq, 10, 1 )
VOLT_ASSIGN( PercentEq, 10, 1 )
VOLT_ASSIGN( PowEq, 10, 1 )
VOLT_ASSIGN( AmpEq, 10, 1 )
VOLT_ASSIGN( PipeEq, 10, 1 )
VOLT_ASSIGN( CaretEq, 10, 1 )
VOLT_ASSIGN( ShlEq, 10, 1 )
VOLT_ASSIGN( ShrEq, 10, 1 )

// Ternary `?:` is handled specially in ParseExpr but needs a binding power.
VOLT_INFIX( Question, 15, 1 )

// Pipes: `a |> f` is left-associative (`a |> f |> g` == `g(f(a))`); the
// mirrored `f <| a` is right-associative (`f <| g <| a` == `f(g(a))`).
VOLT_INFIX( PipeGreater, 18, 0 )
VOLT_INFIX( LessPipe, 18, 1 )

// Key/value pair (`k => v`) — used in hash literals and DSL calls.
VOLT_INFIX( FatArrow, 12, 0 )

// Range.
VOLT_INFIX( DotDot, 20, 0 )
VOLT_INFIX( Ellipsis, 20, 0 )

// Logical.
VOLT_INFIX( OrOr, 30, 0 )
VOLT_INFIX( KwOr, 30, 0 )
VOLT_INFIX( AndAnd, 35, 0 )
VOLT_INFIX( KwAnd, 35, 0 )

// Equality / comparison.
VOLT_INFIX( TripleEq, 40, 0 )
VOLT_INFIX( EqEq, 40, 0 )
VOLT_INFIX( NotEq, 40, 0 )
VOLT_INFIX( Lt, 50, 0 )
VOLT_INFIX( Gt, 50, 0 )
VOLT_INFIX( Le, 50, 0 )
VOLT_INFIX( Ge, 50, 0 )
VOLT_INFIX( Spaceship, 50, 0 )

// Bitwise.
VOLT_INFIX( Pipe, 55, 0 )
VOLT_INFIX( Caret, 56, 0 )
VOLT_INFIX( Amp, 57, 0 )
VOLT_INFIX( Shl, 60, 0 )
VOLT_INFIX( Shr, 60, 0 )

// Arithmetic.
VOLT_INFIX( Plus, 70, 0 )
VOLT_INFIX( Minus, 70, 0 )
VOLT_INFIX( Star, 80, 0 )
VOLT_INFIX( Slash, 80, 0 )
VOLT_INFIX( Percent, 80, 0 )
VOLT_INFIX( Pow, 90, 1 )

// Prefix operators that lower to a Unary node (`*p` is Deref — structural).
VOLT_PREFIX( Minus )
VOLT_PREFIX( Plus )
VOLT_PREFIX( Bang )
VOLT_PREFIX( Tilde )
VOLT_PREFIX( KwNot )
VOLT_PREFIX( Amp )

#undef VOLT_INFIX
#undef VOLT_ASSIGN
#undef VOLT_PREFIX
