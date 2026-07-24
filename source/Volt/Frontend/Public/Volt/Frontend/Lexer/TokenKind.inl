// TokenKind.inl — the single source of truth for the token vocabulary.
//
// Re-included with different definitions of the three macros below to generate
// the enum, the spelling table, and the keyword lookup. Adding a token is one
// line here.
//
//   VOLT_TOKEN(Name)              dynamic lexeme (literals, identifiers, ...)
//   VOLT_PUNCT(Name, "spelling")  fixed punctuation / operator
//   VOLT_KEYWORD(Name, "spelling") reserved word (matched as an identifier)

#ifndef VOLT_TOKEN
    #define VOLT_TOKEN( Name )
#endif
#ifndef VOLT_PUNCT
    #define VOLT_PUNCT( Name, Spelling )
#endif
#ifndef VOLT_KEYWORD
    #define VOLT_KEYWORD( Name, Spelling )
#endif

// --- Dynamic-lexeme tokens -------------------------------------------------
VOLT_TOKEN( Eof )
VOLT_TOKEN( Error )
VOLT_TOKEN( Newline )
VOLT_TOKEN( Identifier )  // lower-case-initial name, may end in ? or !
VOLT_TOKEN( Constant )    // upper-case-initial name (types, namespaces)
VOLT_TOKEN( InstanceVar ) // @name
VOLT_TOKEN( IntLiteral )
VOLT_TOKEN( FloatLiteral )
VOLT_TOKEN( StringLiteral )
VOLT_TOKEN( CharLiteral )
VOLT_TOKEN( SymbolLiteral ) // :name

// --- Punctuation & operators (longest match wins in the lexer) -------------
VOLT_PUNCT( LParen, "(" )
VOLT_PUNCT( RParen, ")" )
VOLT_PUNCT( LBracket, "[" )
VOLT_PUNCT( RBracket, "]" )
VOLT_PUNCT( LBrace, "{" )
VOLT_PUNCT( RBrace, "}" )
VOLT_PUNCT( Comma, "," )
VOLT_PUNCT( Semicolon, ";" )
VOLT_PUNCT( Ellipsis, "..." )
VOLT_PUNCT( DotDot, ".." )
VOLT_PUNCT( Dot, "." )
VOLT_PUNCT( ColonColon, "::" )
VOLT_PUNCT( Colon, ":" )
VOLT_PUNCT( Arrow, "->" )
VOLT_PUNCT( FatArrow, "=>" )
VOLT_PUNCT( Spaceship, "<=>" )
VOLT_PUNCT( ShlEq, "<<=" )
VOLT_PUNCT( ShrEq, ">>=" )
VOLT_PUNCT( Shl, "<<" )
VOLT_PUNCT( Shr, ">>" )
VOLT_PUNCT( Le, "<=" )
VOLT_PUNCT( Ge, ">=" )
VOLT_PUNCT( TripleEq, "===" )
VOLT_PUNCT( EqEq, "==" )
VOLT_PUNCT( NotEq, "!=" )
VOLT_PUNCT( Lt, "<" )
VOLT_PUNCT( Gt, ">" )
VOLT_PUNCT( PowEq, "**=" )
VOLT_PUNCT( Pow, "**" )
VOLT_PUNCT( PlusEq, "+=" )
VOLT_PUNCT( MinusEq, "-=" )
VOLT_PUNCT( StarEq, "*=" )
VOLT_PUNCT( SlashEq, "/=" )
VOLT_PUNCT( PercentEq, "%=" )
VOLT_PUNCT( AmpEq, "&=" )
VOLT_PUNCT( PipeEq, "|=" )
VOLT_PUNCT( CaretEq, "^=" )
VOLT_PUNCT( AndAnd, "&&" )
VOLT_PUNCT( OrOr, "||" )
VOLT_PUNCT( Plus, "+" )
VOLT_PUNCT( Minus, "-" )
VOLT_PUNCT( Star, "*" )
VOLT_PUNCT( Slash, "/" )
VOLT_PUNCT( Percent, "%" )
VOLT_PUNCT( AmpDot, "&." )
VOLT_PUNCT( Amp, "&" )
VOLT_PUNCT( Pipe, "|" )
VOLT_PUNCT( PipeGreater, "|>" )
VOLT_PUNCT( LessPipe, "<|" )
VOLT_PUNCT( Caret, "^" )
VOLT_PUNCT( Tilde, "~" )
VOLT_PUNCT( Bang, "!" )
VOLT_PUNCT( Question, "?" )
VOLT_PUNCT( Assign, "=" )
VOLT_PUNCT( At, "@" )

// --- Keywords --------------------------------------------------------------
VOLT_KEYWORD( KwCase, "case" )
VOLT_KEYWORD( KwWhen, "when" )
VOLT_KEYWORD( KwThen, "then" )
VOLT_KEYWORD( KwModule, "module" )
VOLT_KEYWORD( KwClass, "class" )
VOLT_KEYWORD( KwStruct, "struct" )
VOLT_KEYWORD( KwEnum, "enum" )
VOLT_KEYWORD( KwMixin, "mixin" )
VOLT_KEYWORD( KwDef, "def" )
VOLT_KEYWORD( KwMacro, "macro" )
VOLT_KEYWORD( KwDo, "do" )
VOLT_KEYWORD( KwEnd, "end" )
VOLT_KEYWORD( KwFor, "for" )
VOLT_KEYWORD( KwIn, "in" )
VOLT_KEYWORD( KwSelf, "self" )
VOLT_KEYWORD( KwInclude, "include" )
VOLT_KEYWORD( KwAbstract, "abstract" )
VOLT_KEYWORD( KwExternal, "external" )
VOLT_KEYWORD( KwComponent, "component" )
VOLT_KEYWORD( KwCircuit, "circuit" )
VOLT_KEYWORD( KwSuper, "super" )
VOLT_KEYWORD( KwIf, "if" )
VOLT_KEYWORD( KwElsif, "elsif" )
VOLT_KEYWORD( KwElse, "else" )
VOLT_KEYWORD( KwUnless, "unless" )
VOLT_KEYWORD( KwWhile, "while" )
VOLT_KEYWORD( KwUntil, "until" )
VOLT_KEYWORD( KwRaise, "raise" )
VOLT_KEYWORD( KwBegin, "begin" )
VOLT_KEYWORD( KwRescue, "rescue" )
VOLT_KEYWORD( KwEnsure, "ensure" )
VOLT_KEYWORD( KwReturn, "return" )
VOLT_KEYWORD( KwBreak, "break" )
VOLT_KEYWORD( KwNext, "next" )
VOLT_KEYWORD( KwSizeOf, "sizeof" )
VOLT_KEYWORD( KwOf, "of" )
VOLT_KEYWORD( KwTrue, "true" )
VOLT_KEYWORD( KwFalse, "false" )
VOLT_KEYWORD( KwNil, "nil" )
VOLT_KEYWORD( KwAnd, "and" )
VOLT_KEYWORD( KwOr, "or" )
VOLT_KEYWORD( KwNot, "not" )

#undef VOLT_TOKEN
#undef VOLT_PUNCT
#undef VOLT_KEYWORD
