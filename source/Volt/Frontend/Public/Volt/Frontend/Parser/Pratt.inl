// Pratt.inl — infix operator binding-power table.
//
//   VOLT_INFIX( TokenName, LeftBindingPower, RightAssociative )
//
// Higher binding power binds tighter. Right-associative operators recurse at
// their own level; left-associative ones recurse one level higher. Prefix and
// postfix operators are handled directly in ParseExpr.cpp.

#ifndef VOLT_INFIX
    #define VOLT_INFIX( Name, Lbp, RAssoc )
#endif

// Assignment (right-associative, lowest precedence).
VOLT_INFIX( Assign, 10, 1 )
VOLT_INFIX( PlusEq, 10, 1 )
VOLT_INFIX( MinusEq, 10, 1 )
VOLT_INFIX( StarEq, 10, 1 )
VOLT_INFIX( SlashEq, 10, 1 )
VOLT_INFIX( PercentEq, 10, 1 )
VOLT_INFIX( PowEq, 10, 1 )
VOLT_INFIX( AmpEq, 10, 1 )
VOLT_INFIX( PipeEq, 10, 1 )
VOLT_INFIX( CaretEq, 10, 1 )
VOLT_INFIX( ShlEq, 10, 1 )
VOLT_INFIX( ShrEq, 10, 1 )

// Ternary `?:` is handled specially in ParseExpr but needs a binding power.
VOLT_INFIX( Question, 15, 1 )

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

#undef VOLT_INFIX
