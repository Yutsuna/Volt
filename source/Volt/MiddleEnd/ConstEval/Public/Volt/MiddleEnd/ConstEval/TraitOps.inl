// TraitOps.inl — the manifest of receiver traits: the compile-time questions
// a program asks *about a value* (`user.is_a? Admin`, `row.has_field? :id`).
//
// Re-included with a different definition of VOLT_TRAIT to generate the
// per-trait evaluator declarations, the dispatch table, and the spelling
// index. Adding a trait is one line here plus one `Trait<Name>` definition in
// TraitEngine.cpp — no new AST node, no new branch at the interception seam,
// and nothing in any backend.
//
//   VOLT_TRAIT( Name, Token, Operand )
//     Name     the evaluator's suffix — `Trait##Name` in TraitEngine.cpp.
//     Token    the TokenKind row backing it. Every one of these is a
//              VOLT_TRAIT_KEYWORD row of TokenKind.inl, which is what the
//              parser reads to decide a paren-less argument may follow. That
//              file owns the *spellings*; this one owns the *meanings*, and a
//              static_assert in TraitEngine.cpp pins the two lists to the
//              same length so neither can grow without the other.
//     Operand  EOperandKind — how the single argument is written:
//                Type  a type name    (`obj.is_a? Admin`)
//                Name  a symbol       (`obj.has_field? :email`)

#ifndef VOLT_TRAIT
    #define VOLT_TRAIT( Name, Token, Operand )
#endif

//          Name          Token           Operand
VOLT_TRAIT( Includes, KwIncludes, Type )
VOLT_TRAIT( InheritsFrom, KwInheritsFrom, Type )
VOLT_TRAIT( IsA, KwIsA, Type )
VOLT_TRAIT( HasField, KwHasField, Name )
VOLT_TRAIT( HasMethod, KwHasMethod, Name )

#undef VOLT_TRAIT
