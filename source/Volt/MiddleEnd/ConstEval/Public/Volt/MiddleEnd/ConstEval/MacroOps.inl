// MacroOps.inl — the manifest of member operations available on compile-time
// macro values (`` `uname`.trim ``, `self.fields.size`).
//
// Re-included with a different definition of VOLT_MACRO_OP to generate the
// per-op function declarations and the spelling → function table. Adding an
// op = one line here + one Op<Name> definition in MacroValue.cpp.
//
// The spellings are the *macro evaluator's* own vocabulary — the closed set of
// operations the compiler can carry out on a compile-time value — not Volt type
// or member names it went looking for. Nothing here is resolved against the
// stdlib, and a value that is not compile-time never reaches this table: the
// expression is emitted as ordinary runtime code and resolved by the type
// checker like any other (rules/zero-hardcode.md).
//
//              Op     Spelling
#ifndef VOLT_MACRO_OP
    #define VOLT_MACRO_OP( Name, Spelling )
#endif

VOLT_MACRO_OP( Size, "size" )
VOLT_MACRO_OP( Lines, "lines" )
VOLT_MACRO_OP( Trim, "trim" )
VOLT_MACRO_OP( Chomp, "chomp" )
VOLT_MACRO_OP( Basename, "basename" )
// Introspection of the target type, the second compile-time source after a
// command literal. A receiver these do not fit is not an error but a plain
// runtime member access: `"text".name` stays the call the source wrote, and
// the type checker resolves it like any other (see ApplyMacroOp's contract).
VOLT_MACRO_OP( Fields, "fields" )
VOLT_MACRO_OP( Name, "name" )
VOLT_MACRO_OP( Type, "type" )

#undef VOLT_MACRO_OP
