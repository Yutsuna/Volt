// MagicConstants.inl — the single manifest of compiler-injected constants.
//
// Re-included with a different definition of VOLT_MAGIC to generate both the
// expansion body and the known-name table. Adding a constant is one line here
// and nothing else: no new AST node, no new token, no extra branch.
//
//   VOLT_MAGIC( Spelling, Node, Value )
//     Spelling  the source text, as a *string literal* — deliberately not a
//               bare identifier, since __FILE__ / __LINE__ are C preprocessor
//               macros and would expand if written nude as a macro argument.
//     Node      the Frontend AST literal node it lowers to. Every literal
//               usable here has the shape { SourceRange, Symbol }, which is
//               what lets one construction expression cover all of them.
//     Value     an expression over `Site` (a MagicSite) or any constant,
//               rendered through MagicText — std::string_view or std::uint32_t.
//
// The lowering targets ordinary literal nodes on purpose: the Volt type then
// falls out of the existing zero-hardcode binding (@[Literal( StringLiteral )]
// on String, @[Literal( IntLiteral )] on Int32). No Volt type name here.

#ifndef VOLT_MAGIC
    #define VOLT_MAGIC( Spelling, Node, Value )
#endif

//          Spelling         Node           Value
VOLT_MAGIC( "__FILE__", StringLiteral, Site.Path )
VOLT_MAGIC( "__DIR__", StringLiteral, Site.Dir )
VOLT_MAGIC( "__LINE__", IntLiteral, Site.Line )
VOLT_MAGIC( "__COLUMN__", IntLiteral, Site.Column )
VOLT_MAGIC( "__FUNCTION__", StringLiteral, Site.Function )
VOLT_MAGIC( "__METHOD__", StringLiteral, Site.Function )
VOLT_MAGIC( "__VERSION__", StringLiteral, Core::VoltVersion )

#undef VOLT_MAGIC
