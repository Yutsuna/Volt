// MacroCalls.inl — the manifest of *call shapes* the macro evaluator carries
// out itself, as opposed to MacroOps.inl's operations on a value.
//
// The difference is what the evaluator does with the node, not where it sits:
// an operation folds a receiver into another value (`` `uname`.trim ``), while
// a call here is a compile-time *action* whose shape the evaluator must
// recognise — a loop it unrolls, a line it prints on the compiler's console.
// Neither ever reaches a backend, and a call the manifest does not list is
// emitted verbatim into the generated method, arguments folded, and resolved
// by the type checker like any other (`assert!( 1024 > 0 )`).
//
// Re-included with a different definition of VOLT_MACRO_CALL to generate the
// enumeration and the spelling table. Adding a shape = one line here + the arm
// that carries it out.
//
// The spellings are the macro evaluator's own closed vocabulary, not Volt type
// or member names it went looking for (rules/zero-hardcode.md — the same
// argument MacroOps.inl's header makes at greater length).
//
//                Name   Spelling
#ifndef VOLT_MACRO_CALL
    #define VOLT_MACRO_CALL( Name, Spelling )
#endif

// `for field in self.fields` — the parser desugars every `for` into
// `seq.each { |field| ... }` (ParseStmt.cpp), so this spelling is the one
// shape compile-time iteration can possibly arrive in. A receiver that is not
// a compile-time sequence is an ordinary runtime loop, emitted as written.
VOLT_MACRO_CALL( Each, "each" )

// `puts "..."` — output on the *compiler's* console, at compile time. This is
// what tells `puts` (an action of the compiler) apart from `assert!( ... )`
// (a call in the generated program), with no keyword to mark either.
VOLT_MACRO_CALL( Puts, "puts" )

#undef VOLT_MACRO_CALL
