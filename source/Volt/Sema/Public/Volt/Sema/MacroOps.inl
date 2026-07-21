// MacroOps.inl — the manifest of member operations available on compile-time
// macro values (`{{ fields.size }}`).
//
// Re-included with a different definition of VOLT_MACRO_OP to generate the
// per-op function declarations and the spelling → function table. Adding an
// op = one line here + one Op<Name> definition in MacroExpansion.cpp. The
// spellings are part of the macro template DSL, not Volt type names.
//
//              Op     Spelling
#ifndef VOLT_MACRO_OP
    #define VOLT_MACRO_OP( Name, Spelling )
#endif

VOLT_MACRO_OP( Size, "size" )

#undef VOLT_MACRO_OP
