 Now the pipeline is fully clear:

   1. **parse** (parallel): ParseOne per unit
   2. **seam.interface** (serial): PublishUnitInterface + BindUnitTypes per unit → ResolveStructLayouts → SynthesizeFinalizeStubs → ResolveUnitSignatures → ComputeAllVTableSlots +
   ComputeSubtypeIntervals. **After this seam, the TypeStore is frozen.**
   3. **sema.lowering** (parallel): RunSemaLoweringOne = RunPasses(min, LoweredSeamOrder()) — passes order < seam: FunctionalLowering(8), PipelineLowering(9), ScopeResolver(10),
   MacroExpansion(15), MagicExpansion(16), JsxLowering(20), CaseLowering(22), DotCallLowering(23), AssignLowering(24), IndexLowering(25), InterpLowering(26).
   4. RAII ownership seam (serial, between the halves)
   5. **sema.typed** (parallel): RunSemaTypedOne = RunPasses(LoweredSeamOrder(), max) — TypeChecker(30), UnusedChecker(35), AstInvariant(40).

   KEY INSIGHT for the ordering question: **MacroExpansion (order 15) runs AFTER the interface seam**, so the frozen TypeStore already knows every type's fields (Members with EMemberKind::Field),
   layouts (Aggregate Fields), includes, supers, and resolved signatures. This means `self.fields` introspection is implementable at order 15 by reading `Context.Types` — NO reordering needed for
   field introspection! The trap described in the issue ("fields are resolved during BindUnitTypes") is actually already solved because BindUnitTypes runs before ALL passes.

   BUT there's a real subtlety: TypeBinder Phase A "freezes each type's member list" from the AST *as parsed*. If a `macro def to_json` inside a mixin generates a NEW method for the including
   class at pass time (order 15), that generated method is NOT in the TypeStore's member list (the store is frozen). Consequences:
   - The generated method won't be callable from OTHER units via Member lookup... actually within the same unit, TypeChecker resolves calls via TypeStore members (LookupMemberOn). If `to_json`
   isn't a Member of User in the TypeStore, `user.to_json` won't resolve.
   - So macro-generated methods must be REGISTERED in the TypeStore. But PassContext.Types is `const TypeStore &`!

   Hmm. How does the current MacroInvoke expansion handle this? Current system: `macro def` bodies are text templates, `MacroInvoke` in a class body expands to decls (e.g., a `def` method) at
   order 15 — AFTER the seam. So those generated methods are ALSO not registered in the TypeStore?! Let me check how that works today... Maybe TypeChecker has a fallback, or maybe the current
   macro system only generates things that don't need member registration. Let me check the samples using macro and how TypeChecker resolves generated methods.

   Actually wait — let me re-check. Does the Driver comment at line 345-348 say "Must run before BindUnitTypes below (the cross-unit seam that builds the free-function table from Ast.TopDecls): a
   Method a Sema pass adds later would never be found by name (see PointFreeLowering.hpp)". So indeed: a Method added by a sema pass (order ≥8) after the seam would NEVER be found by name. This is
   a REAL ordering wall for macro def generating methods.

   So for issue #75, `macro def to_json` inside `mixin Serializable`, included by `class User` — the expansion generates `def to_json` inside User's body at order 15, after the TypeStore froze.
   The method wouldn't be found by name via LookupMemberOn.

   Options:
   A. Move macro expansion BEFORE the seam — i.e., expand macros at parse time / right after parse, before BindUnitTypes. But then `self.fields` can't be answered... unless the expansion for
   type-introspecting macros is deferred.
   B. Two-phase macro system: (1) macro do blocks + backtick constants + MacroInvoke (file-level, no type introspection) expand at order 15 as today; (2) `macro def` methods inside mixins/classes
   that need `self.fields` are NOT expanded at order 15 — instead, they're expanded at the SERIAL SEAM, after BindUnitTypes+ResolveStructLayouts but before the store freezes... Actually the seam
   is where the store is still mutable (BindUnitTypes takes `TypeStore &`). A new seam step "ExpandMacroDefs" could run after ResolveUnitSignatures: it walks every type that includes a mixin with
   macro defs, evaluates the macro body (const-eval engine with `self` bound to the including type's NominalId), parses/emits the generated Method decls into the class Body, and REGISTERS them as
   Members via the mutable TypeStore. That keeps TypeChecker unchanged and preserves every invariant (sugar gone before TypeChecker; members registered before freeze).

   This is the clean answer to the "mur d'ordonnancement": macro def expansion is a SEAM step, not a per-unit pass, because it needs the frozen-ish type info AND must publish new members. The seam
   is exactly where mutable TypeStore access exists.

   Hmm wait, but there's a subtlety: the macro body is Volt code evaluated at compile time. It references `self.fields`. At the seam, after BindUnitTypes, the including type's Members are known
   (Fields). So `self.fields` = introspection over `NominalType.Members` filtered by EMemberKind::Field (+ inherited? — design question). The macro evaluator emits AST (via quasi-quotation? or by
   building strings and re-parsing?).

   The issue's design says: "le corps de la macro inspecte la structure du type cible (ex: self.fields) et émet/synthétise directement l'AST de l'implémentation concrète". With `@#{field.name}`
   interpolation of identifiers. And `json += "...#{field.name}..."` — wait, looking at the example:

   ```vl
   macro def to_json -> String
     json = "{"
     for field in self.fields
       json += "\"#{field.name}\":" + @#{field.name}.to_string + ","
     end
     json.chomp(",") + "}"
   end
   ```

   Hmm, this is ambiguous. Two readings:
   1. The macro body is compile-time code that builds a STRING of Volt source (or AST), where `for field in self.fields` iterates at compile time, `#{field.name}` interpolates the field's name,
   and `@#{field.name}` splices an instance-var access into the generated code. The resulting string/AST becomes the body of `to_json`.
   2. The macro body is an AST template where compile-time parts (`for field in self.fields`) are evaluated and runtime parts are quoted.

   In reading 1, `json` is a compile-time String variable; the loop appends text like `"name:" + @name.to_string + ","`... no wait, `@#{field.name}.to_string` — if this were text concat, `json +=
   "\"name\":" + @name.to_string + ","` would evaluate `@name.to_string` at compile time, which is nonsense.

   In reading 2 (Crystal-style), the body is a mix: compile-time `for` unrolls the loop, and each iteration emits the expression `"\"#{field.name}\":" + @#{field.name}.to_string + ","`... hmm no.

   Crystal's macro would be:
   ```crystal
   macro def to_json : String
     json = "{"
     {% for field in self.fields %}  # no — Crystal uses @type.instance_vars
       ...
     {% end %}
   end
   ```

   Actually in the issue's design (point 4: "Pas de macro if ou macro for dédiés — les if/for Volt standards sont exécutés directement par le moteur d'évaluation compile-time"), the ENTIRE body is
   evaluated at compile time, and the RESULT (a value, presumably AST or string) is emitted. So in `macro def to_json`, the body builds a compile-time string `json` containing the generated source
   text `"name:" + @name.to_string + ","`-shaped text per field... no — wait.

   Look carefully: `json += "\"#{field.name}\":" + @#{field.name}.to_string + ","`. If `json` is compile-time text and the whole RHS must produce TEXT, then `@#{field.name}.to_string` must be TEXT
   "`@name.to_string`" — i.e., `@#{field.name}` interpolates the field name into an identifier-shaped text, and `.to_string` is literal text. But `+ @name.to_string +` — string + string is text
   concat at compile time. So the RHS is: text `"\"name\":"` (string interp evaluates field.name at compile time) + text `@name.to_string` + text `,`. So `@#{field.name}.to_string` is an
   "escaped/quoted" expression that renders as source text rather than evaluating. Hmm, but then how does the engine know `@#{field.name}.to_string` is quoted text rather than code to evaluate?

   Alternatively — the intended semantics might be that the macro body is evaluated as compile-time code, and `@#{field.name}` is an AST-producing expression: it produces an AST node (InstanceVar
   with dynamically-computed name), and `+` on AST nodes concatenates them into an expression list... This is getting into "REAL RESOLUTIONS" territory — the issue title says "with REAL
   RESOLUTIONS", implying the current fake text-template should become real evaluation with real AST emission.

   I think the cleanest semantic consistent with the issue:
   - Inside `macro def`, the body is evaluated by the compile-time interpreter.
   - Values are compile-time values: strings, ints, bools, arrays, and **AST nodes** (quoted code).
   - `self.fields` yields an array of field descriptors `{name, type}` from the TypeStore (no hardcode: names come from the store, rendered via the store's interner).
   - `#{...}` inside a string interpolates compile-time values as text.
   - `@#{field.name}` is sugar for splicing a dynamic identifier: it yields a quoted InstanceVar AST node whose name is computed at compile time.
   - The body's final value must be a compile-time String (source text to re-parse) or a sequence of AST nodes. The simplest v1: the body evaluates to a String of Volt source, which is
   re-lexed/re-parsed into real AST (same approach as today's expander — reuse the re-parse machinery, which already stamps locations to the invocation site).

   That gives "REAL RESOLUTIONS": real Volt control flow (if/for/while, case), real method calls on compile-time values (String methods like trim/lines/chomp/basename — via a MacroOps manifest or
   a real mini-interpreter), real shell backticks, and real TypeStore introspection — while the emission mechanism stays "generate text → re-parse", which is proven in the current code and avoids
   a whole quasi-quotation AST API.

   But wait — the example `@#{field.name}.to_string` inside a string-concat context: under the "body builds a String" model, `"\"#{field.name}\":"` is a String, `@#{field.name}.to_string` must
   also be a String. So we need a rule: an *identifier-shaped expression* (`@name`, `foo`, `foo.bar`) evaluated in compile-time concat position... no, too magic.

   Better: introduce **code quotation**: `@#{field.name}` produces an `AstValue` (quoted expression node); `String + AstValue` stringifies the AST node back to source text. That's a real, explicit
   semantic: AST values stringify to their source form. `.to_string` on an AstValue also yields its source text. Hmm, `@#{field.name}.to_string` would then parse as `(@#{field.name}).to_string` =
   AstValue.to_string = "@name" — WRONG, we want `@name.to_string` (the runtime call on the instance var!).

   Ugh. The example is genuinely ambiguous. Let me reconsider: maybe the intended reading is that the *whole line* `json += "\"#{field.name}\":" + @#{field.name}.to_string + ","` is EMITTED as
   code text per loop iteration — i.e., the `for field in self.fields ... end` is a compile-time loop whose body statements are EMITTED (not executed), with `#{...}` interpolated at emission time.
   That's the "template" reading: everything is emitted as text except `#{...}` holes and the control flow. But then `json = "{"` and `json.chomp(",") + "}"` would also be emitted as-is — which is
   exactly what we want for the generated `to_json` body! The generated method body is:

   ```vl
   json = "{"
   json += "\"name\":" + @name.to_string + ","
   json += "\"age\":" + @age.to_string + ","
   json.chomp(",") + "}"
   ```

   YES. That's it. The macro body is a **compile-time template with real control flow**: `for field in self.fields` is evaluated at compile time (unrolled), and each non-control-flow statement is
   emitted verbatim (as text/AST), with `#{...}` holes evaluated at compile time and spliced. This is reading 2 (Crystal-style) but with unified syntax: no `{% %}` tags; instead, the distinction
   is by construct kind:
   - `for ... in self.fields` where the iterable is a compile-time value → unroll at compile time, emit body per iteration.
   - `if <compile-time condition>` → emit only the taken branch.
   - `#{expr}` inside a string or `@#{expr}` → evaluate expr at compile time, splice.
   - Everything else → emit as-is into the generated method body.

   And `if `uname`.trim == "Linux"` — the condition is compile-time evaluable (backtick → String literal value), so only one branch is emitted. If the condition is NOT compile-time evaluable →
   error? or emit a runtime if? Design decision: inside `macro def`, `if/for` are ALWAYS compile-time (that's the issue's stated rule); conditions must be compile-time evaluable, else diagnostic.

   And in the `register_test_suite` example:
   ```vl
   macro def register_test_suite -> Void
     test_files = `find ...`.lines     # compile-time: backtick → Array<String>
     for file in test_files            # compile-time loop over compile-time array
       test_name = file.basename(".vl") # compile-time String op
       puts "Generating test fixture for #{test_name}..."  # compile-time puts → compiler stdout
       content = `cat #{file}`          # compile-time
       assert!( content.size > 0 )      # compile-time assert
     end
   end
   ```
   Here NOTHING is emitted — the body is purely executed for side effects at compile time. So `macro def` has dual nature: statements either execute at compile time (assignments of compile-time
   values, puts, assert) or emit code... How to distinguish `json = "{"` (emit) from `test_files = ...` (execute)?

   Hmm. In the to_json example, `json = "{"` — is json a compile-time variable or an emitted runtime local? The final line `json.chomp(",") + "}"` is the method's return value → runtime. If `json`
   were compile-time, the emitted body would be just a final string literal. Both readings produce valid programs! The difference matters for `@#{field.name}`: under "emit" reading,
   `@name.to_string` is emitted runtime code. Under "execute" reading, json would be pure text built at compile time: `json += "\"name\":" + @name.to_string + ","` — but then `@name.to_string`
   executes at compile time, which is impossible.

   So the coherent semantic MUST be the template/emission reading:
   - The body is a compile-time program whose *values* include "quoted code" implicitly: any expression that is not compile-time evaluable (references `@ivar`, runtime method calls on runtime
   receivers, runtime locals) is **quoted/emitted** rather than executed.
   - Control flow (`if`, `for`) over compile-time values is executed (unrolled/selected).
   - Assignments of compile-time values create compile-time locals; assignments involving runtime values emit runtime locals.

   That's essentially "partial evaluation": the macro evaluator tries to const-eval every expression; anything touching runtime-only data (`@ivar`, `self` as the runtime receiver, params of the
   generated method) becomes quoted AST. This is exactly how a staged interpreter works (like Terra/Rumble). It's implementable as: the eval engine has a value domain
   `Const(String|Int|Bool|Array|...) | Quoted(ExprId)`. `+` on two Consts folds; on any Quoted operand, builds a `Binary` AST node. `#{...}` in a string literal inside a macro: if operand is Const
   → text splice; if Quoted → emits an `Interp` node... wait no — in the example, `#{field.name}` appears in the STRING being built at compile time...

   Hold on. Re-read once more:

   ```vl
   macro def to_json -> String
     json = "{"
     for field in self.fields
       json += "\"#{field.name}\":" + @#{field.name}.to_string + ","
     end
     json.chomp(",") + "}"
   end
   ```

   Under the partial-eval reading:
   - `json = "{"` → Const("{")
   - Loop iteration for field=name: `json += "\"#{field.name}\":" + @#{field.name}.to_string + ","`
     - `"\"#{field.name}\":"` → Const("\"name\":") (field.name is Const)
     - `@#{field.name}` → Quoted(InstanceVar("name")) (an ivar is runtime)
     - `.to_string` on Quoted → Quoted(Call(Member(InstanceVar("name"), "to_string")))
     - Const + Quoted → Quoted(Binary(Const, +, Quoted))
     - `json += <Quoted>` → json becomes Quoted(Assign...)? Hmm, `json` was Const("{") and now becomes Quoted — meaning the emitted code must materialize `json` as a runtime local: emit `json =
   "{"` statement, then `json += ...` statements.

   So the rule: when a compile-time local transitions from Const to Quoted, the evaluator must emit the prior assignments as runtime statements. This is doable but subtle (requires tracking
   "pending emission" per variable). Simpler rule used by real systems (e.g. Zig comptime is different; Crystal macros are pure text; D's CTFE+mixin is two-step)...

   OK. I'm the architect; the issue is a design proposal with ambiguity, and my job is to produce an execution plan with explicit "Open Questions & Arbitrages" for PM validation. I should pick ONE
   coherent semantic as the recommendation and list alternatives. Let me define the recommended semantics crisply, optimized for implementability within Volt's architecture:

   **Recommended semantics — "staged template with compile-time string building" (two zones):**

   Actually, let me step back and think about what is *implementable* in Volt with ~minimal complexity and maximal conformance to the issue text, and matches the existing infrastructure
   (MacroValue model, re-parse emission, TypeStore introspection).

   The existing engine already: scans text, evaluates `{% for %}`/`{% if %}`/`{{ }}` over MacroValues, emits text, re-parses. Issue #75 asks to replace the `{% %}`/`{{ }}` DSL with REAL Volt code
   (real `for`/`if` AST, real method calls like `.trim`, `.lines`, `.basename`, `.chomp`), add backticks, add `self.fields` introspection, add `macro do`, add global const backticks.

   The cleanest architecture that satisfies "real resolutions" without inventing a full staged partial evaluator:

   **Zone model**: a `macro def` body is REAL Volt, parsed once into ordinary AST (no more raw text slicing). The compile-time evaluator executes it with a value domain:
   - `CValue = Bool | Int | String | Array<CValue> | AstQuote(ExprId)`
   - Statements execute in order. `if`/`while`/`for` evaluate their condition/iterable at compile time (must be CValue, else error "macro control flow must be compile-time").
   - Expressions evaluate to CValue when all inputs are CValue; otherwise produce AstQuote (quoted core AST).
   - The **return contract**: the macro body's value (last expression) must be either:
     - a `String` CValue → re-lexed/re-parsed as the member block / statements to splice (today's proven path), or
     - an AstQuote → spliced directly.

   Hmm, but the to_json example under this model: `json` starts Const, then `json += ...Quoted...` → json becomes AstQuote. `json.chomp(",")` → Quoted call. Final value = AstQuote(Binary(...)) —
   an EXPRESSION, but the generated body also needs the preceding `json = "{"` statement and the unrolled `json += ...` statements...

   This is the classic problem. Solutions in real languages:
   - Crystal: macro bodies are text templates with explicit {% %} control — the "json" in the example would be a RUNTIME local emitted as text; loop unrolling happens textually.
   - The issue's to_json example works PERFECTLY under Crystal semantics: everything is text emission; `for field in self.fields` unrolls; `#{...}` splices. `json` is runtime.

   And the register_test_suite example: `test_files = ...` — hmm, under pure text semantics, `test_files` would be emitted as a runtime local and `.lines` a runtime call — but the comment says
   "Executed on host during compilation". Under Crystal semantics you'd write `{% test_files = ... %}` explicitly. The issue deliberately removed `{% %}`, creating this ambiguity. UNLESS: the
   distinguishing rule is data-flow: `for file in test_files` requires `test_files` to be a compile-time value (you can't unroll over a runtime array), so `test_files` is compile-time; `for field
   in self.fields` requires `self.fields` compile-time; `json += ...` — `json` never feeds a compile-time construct, so... it's runtime? That requires a demand/backward analysis — too clever,
   fragile.

   DECISION (architect's call, to validate with PM): **Explicit-quote rule, dual interpretation by construct:**
   Inside a `macro def` body, evaluation is compile-time FIRST (eager): every expression is const-evaluated. A construct is EMITTED (quoted into the generated AST) iff it cannot be fully
   const-evaluated because it references the *runtime receiver* (`@ivar`, `self`, `super`, or the generated method's runtime params). Compile-time locals hold CValues. When an emitted expression
   references a compile-time local, its CValue is spliced as a literal. When a compile-time local is REASSIGNED with an emitted (quoted) value, the evaluator emits the variable's materialization
   (prior pending assignments flush as runtime statements). `for`/`if`/`while`/`case` with compile-time conditions are executed; with any runtime component → hard error (per issue point 4: control
   flow is the compile-time steering mechanism).

   Under this rule:
   - to_json: `json = "{"` → Const. Loop 1: `json += "\"name\":" + Quoted(@name.to_string) + ","` → RHS quoted → emit pending `json = "{"`, then emit `json += ...`; json now "runtime local"
   marker. Loop 2: emit `json += ...`. Final: `json.chomp(",") + "}"` → runtime expr on runtime local → emit as last expression (return value). Generated body = exactly the expected 4 lines. ✓
   - register_test_suite: everything const-evaluable (backticks, .lines, .basename, puts, assert!) → nothing emitted → method body empty → pure compile-time side effects. ✓
   - system_exit: `if `uname`.trim == "Linux"` → Const cond → emit only taken branch (`libc_linux_exit( code )` — a runtime call, quoted). ✓

   This "eager const-eval with quote-on-runtime-reference" is ONE rule, no backward analysis, predictable. The subtle part (flush pending assignments on Const→Quoted transition) is well-scoped.
   I'll spec that.

   `@#{field.name}`: parses as InstanceVar with interpolated name — new sugar: lexer/parser must support `@#{` → produces a `DynamicInstanceVar`-ish node, or more precisely: inside macro bodies
   only, `@#{expr}` is an expression that evaluates expr at compile time to a String and yields Quoted(InstanceVar(computed name)). Similarly `#{...}` inside strings: compile-time splice when the
   whole string is Const, otherwise part of a quoted Interp.

   **Backticks**: new token(s). Lexer: backtick string with interpolation support, mirroring how `"..."` strings with `#{...}` are lexed (need to check how Interp is currently lexed — probably the
   lexer produces StringLiteral tokens and the parser stitches Interp parts, or there are begin/mid/end tokens — must check). New AST node `CommandLit` (sugar, lowered by const-eval at macro
   time... but also usable in global constants: `BUILD_COMMIT : String = `git rev-parse --short HEAD`.trim`). Where is that evaluated? Global constants at top level — TopStmts/LocalDecl with a
   CommandLit init. The const-eval of backticks must happen before TypeChecker (sugar must disappear): MacroExpansion order 15 can evaluate CommandLit anywhere it can const-fold (constant
   position) and replace with StringLiteral. `.trim` on it — a Call on the literal; MagicExpansion-style folding of `.trim`/`.lines` on compile-time strings... but zero-hardcode: `.trim`/`.lines`
   are Volt method names on String — CANNOT be hardcoded in C++!!

   Hmm! `rules/zero-hardcode.md`: no Volt method spelling in C++. But MacroOps.inl already has `VOLT_MACRO_OP( Size, "size" )` with the comment "The spellings are part of the macro template DSL,
   not Volt type names." — that's the sanctioned escape hatch: macro-DSL operation spellings are a compiler-internal DSL, not Volt stdlib member resolution. So `.trim`, `.lines`, `.basename`,
   `.chomp`, `.size` as *compile-time macro ops* in MacroOps.inl is architecturally consistent (they live in the manifest, they're part of the macro DSL vocabulary, like the `size` op today). The
   grep guardrail `! grep -rnE '\b(Int|Int32|UInt64|String|Array|Bool|Proc|Nil)\b'` excludes AST dir and SelfCheck — MacroOps spellings like "size" pass since they're not type names. OK.

   But for `BUILD_COMMIT : String = `git...`.trim` at top level — that expression is typed by TypeChecker AFTER MacroExpansion folds the CommandLit+op into a plain StringLiteral. So the fold must
   happen at order 15 (MacroExpansion) or a dedicated new pass. The `.trim` must fold BEFORE TypeChecker, else TypeChecker sees a Call to String#trim which is... actually fine — String#trim might
   exist in stdlib and would just be a runtime call. But issue says "Constantes globales évaluées au compile-time" — the constant's VALUE is the shell output at compile time. If we only fold the
   backtick to StringLiteral and leave `.trim` as a runtime call, semantics differ (trim at runtime vs compile time) — result is the same VALUE though (pure function). But `.lines` producing
   Array<String> at runtime vs compile-time embedding — the issue wants compile-time. Simplest: MacroExpansion evaluates CommandLit → StringLiteral (compile-time), and the macro-op member chain
   (.trim/.lines/.basename/.chomp/.size) folds when the receiver is a compile-time string value — same MacroOps manifest, reused. Anything not foldable stays a runtime call (still correct, just
   runtime). For global constants, folds guarantee compile-time evaluation. Good.

   Security: ProcessRunner — `Core/Support/ProcessRunner.hpp`: posix_spawn/popen on POSIX, CreateProcess on Windows. Multi-platform per issue. Sandbox/timeout: cap output size, timeout, env
   sanitization, working directory = source file dir? Determinism concern for builds (cache invalidation: frontend cache #61! A cached stdlib unit would not re-run backticks — need to record shell
   commands run + their output in the cache? or forbid backticks in stdlib? Design decision: macro expansions are side-effectful → the frontend cache must key on the command outputs or macros must
   be banned from cached units. Simplest v1: backticks allowed everywhere but the cache stores post-expansion AST (expansion already happened before caching — wait, does the cache store
   parsed-only or post-sema AST? "A cache hit fills Types/Registry/Units[0..StdlibCount) directly, so ParseOne/the seam/RunSemaOne all skip that range" — the cache stores PARSED ast + bound types.
   MacroExpansion runs per unit AFTER... no wait — on cache hit, RunSemaOne SKIPS stdlib units entirely?? "so ParseOne/the seam/RunSemaOne all skip that range below" — hmm, then stdlib units never
   get MacroExpansion run on cache hit? That means macro expansion results must be IN the cache, or stdlib can't use macros. Since macros in stdlib would expand per-build nondeterministically
   (backticks!), the honest v1 rule: stdlib units (cacheable) may not contain backtick/macro-do constructs — enforce by diagnostic in the cache-write path; user units are never cached. I'll put
   this in the plan as a guardrail + open question.)

   **macro do**: top-level `macro do ... end` block → new decl kind `MacroBlock`? or reuse... It's a decl with a StmtList body, evaluated at compile time during MacroExpansion (order 15), then
   dropped from TopDecls (like DropDefsFromRoot). It can contain backticks, puts (compile-time print → compiler stdout diag?), assert! (compile-time check → diag error). Needs `__DIR__` —
   MagicConstants already has it (MagicExpansion order 16 — AFTER MacroExpansion 15; so in macro bodies, `__DIR__` must be evaluable at order 15 — the evaluator handles magic constants itself via
   the same MagicConstants.inl manifest. Good — meta-first: one manifest, two consumers).

   **New nodes**: `CommandLit` (VOLT_EXPR_SUGAR — must die before TypeChecker; fields: Loc, Parts ExprList for interp segments or a Symbol Raw when no interpolation). `MacroBlock` (VOLT_DECL —
   dropped at order 15, never survives; AstInvariant only checks EXPR sugar — MacroDef today is a plain VOLT_DECL and is dropped from TopDecls but stays in arena; same pattern for MacroBlock).
   `macro def` changes: MacroDef gains a real parsed body? The issue wants real Volt in the body → parse the body as ordinary StmtList at parse time. MacroDef struct changes: `StmtList Body`
   replaces `Symbol BodyText`? But the body contains compile-time-only constructs (`self.fields`, `@#{...}`) that are NOT valid ordinary Volt... they're parsed but only meaningful to the
   evaluator. Parser can parse them with new expression nodes: `MacroSelfFields`? No — `self.fields` parses as Member(SelfExpr, "fields") — ordinary nodes! `@#{field.name}` needs a new node
   `DynamicIvar`? Let me think: `@#{expr}` — could parse as `At` + Interp-like. New sugar expr node: `IvarInterp { Loc, ExprId Name }` — evaluated only inside macro bodies; anywhere else → parse
   error or invariant. Simpler: reuse `Interp`? No — Interp is string interpolation producing String. The `@#{name}` yields an InstanceVar node with computed name → new node `DynamicInstanceVar {
   Core::SourceRange Loc; ExprId Name; }` marked VOLT_EXPR_SUGAR, only legal inside macro bodies, eliminated by the macro evaluator.

   What about `self.fields` — ordinary Member(SelfExpr, fields). The evaluator intercepts Member on the macro's `self` (the target type descriptor). But `self` in a macro def also appears in `->
   self` return type (from_json) — that's a TypeRef self, fine.

   Also `puts`/`assert!` inside macro bodies: ordinary Call nodes; the evaluator intercepts them as compile-time ops (MacroOps manifest: puts → Diags/stdout; assert → cond check). Since they're
   Calls to identifiers, the evaluator's Call arm handles known compile-time spellings via MacroOps.inl (meta-first).

   **Field descriptor**: `self.fields` yields Array of CValue-structs? MacroValue needs a "record" shape: field.name, field.type. Model: `MacroValue::Data` gains
   `std::vector<std::pair<std::string, MacroValue>>` (record) or keep simple: a `FieldDesc { std::string Name; std::string Type; }` variant + member ops `.name`/`.type` via MacroOps rows that
   pattern-match the variant. Zero-hardcode: field names come from TypeStore Members (EMemberKind::Field); type rendered how? The SigType → spelling via store (rendered type name string, e.g.
   "String" — as TEXT for codegen, not as C++ semantic). That's fine: rendering a type's declared name as a string is text, not hardcode.

   **Emission**: two modes per macro kind:
   1. `macro def` inside a type/mixin → expansion produces a Method decl whose body = quoted statements accumulated during evaluation; the method is spliced into the including type's Body AND
   registered in TypeStore (needs mutable store → seam problem again!).

   WAIT. Back to the ordering wall. Let me settle it definitively.

   The Driver: seam (serial, mutable TypeStore) → sema.lowering (parallel per unit, const TypeStore) → RAII seam → sema.typed.

   `macro def to_json` in mixin, included by User in another unit possibly. Expansion at order 15 (per-unit, parallel, const store) CAN read User's fields (frozen, complete). But the generated
   Method must be:
   (a) added to User's class Body in User's unit AstContext — cross-unit AST mutation from a parallel pass = FORBIDDEN (passes are per-unit, AllUnits is const span). If mixin and class are in the
   same file, it's local, fine; cross-file include → the expansion must happen in the CLASS's unit, reading the mixin's MacroDef from... the mixin's unit AST (readable via AllUnits — they carry
   `std::span<const Frontend::AstContext *> AllUnits`). MacroDef decl would need cross-unit discovery: TypeStore has NominalType for Serializable with (Unit, Decl) → AllUnits[Unit] → the MacroDef
   in its body. Feasible read-only.
   (b) REGISTERED as a Member of User in the TypeStore — const store, forbidden. ← THE WALL.

   How does TypeChecker resolve `user.to_json` if to_json isn't a Member? It can't. So generated methods MUST be registered. Options:

   **Option A — Seam expansion (recommended)**: Add a serial seam step after ResolveUnitSignatures / before freeze... but expansion needs the frozen member lists (they're complete after
   BindUnitTypes phase A per-type? Members are filled during BindUnitTypes+DefineUnitMembers... let me check: Driver calls BindUnitTypes (Phase A: declare types + members?) then
   ResolveStructLayouts, SynthesizeFinalizeStubs, ResolveUnitSignatures (Phase B: fill SigType Results). When are Members (fields) complete? Need to check TypeBinder.cpp. If fields+layouts are
   complete after ResolveStructLayouts, a new seam step `ExpandMacroMethods(Units, Types, Registry)` right after SynthesizeFinalizeStubs/ResolveUnitSignatures could: for each unit, for each type
   with includes-of-mixins-with-macro-defs (and own macro defs), evaluate macro bodies with self=NominalId, splice generated Methods into the class Body, and AddMember to the mutable store +
   re-resolve signatures for the new members. Then everything downstream (TypeChecker, codegen) is untouched. MacroInvoke expansion (text macros, no type introspection) stays a per-unit pass at
   order 15? No wait — MacroInvoke ALSO generates decls (e.g. `macro def` invoked in a class body generates a method) → same registration wall! How does it work TODAY?? Today, MacroInvoke expands
   at order 15 into Method decls inside a class body — are those registered in TypeStore? They CAN'T be (frozen). So either today's macro system can't generate methods that are callable (only used
   for... let me check samples), or there's a mechanism I haven't seen. Let me check samples + how TypeBinder collects members (maybe it re-scans at TypeChecker time? No...). Let me grep samples
   for macro usage and check whether generated methods are invoked.

   Actually the Driver comment says it explicitly: "a Method a Sema pass adds later would never be found by name (see PointFreeLowering.hpp)". So today, macro-generated methods are NOT findable by
   name — the current macro system is essentially limited to generating... hmm. Let me look at a sample. This is important: Issue #75's title "with REAL RESOLUTIONS" probably means exactly this:
   today macro-generated decls aren't really resolved (not registered, not type-checked properly), and the issue wants real registration/resolution. That's the core architectural fix: move
   expansion to the seam where registration is possible.

   **Option B — Re-bind after expansion**: keep expansion at order 15, then run a second mini-binding seam per unit (re-run BindUnitTypes for mutated types). Breaks the parallel model + cache
   invariants; ugly.

   **Option C — TypeChecker-side resolution**: teach MemberResolver to also look at unregistered AST methods (walk class Body decls not in store). Violates "resolution recorded once on the store"
   architecture; per-call re-derivation; forbidden shape.

   Option A it is: **macro expansion becomes a seam-time serial step** (for macro def methods with type introspection AND for MacroInvoke), positioned right after BindUnitTypes per-unit loop
   completes... but it needs ALL types bound (a class in unit A includes mixin from unit B) → position after the BindUnitTypes loop + ResolveStructLayouts + ResolveUnitSignatures? Generated
   methods need signatures resolved too → after splicing, run signature resolution for the new members (ResolveUnitSignatures is per-unit; re-run for affected units? or make the macro step fill
   SigTypes itself via the same binder helpers). Cleanest: macro seam step sits BETWEEN BindUnitTypes loop and ResolveUnitSignatures loop: types+fields exist (Phase A done), signatures not yet
   resolved → new Method decls spliced in get their signatures resolved by the EXISTING ResolveUnitSignatures loop unchanged. Layouts: ResolveStructLayouts already ran — generated methods don't
   add fields (macro def generates methods; could a macro add fields? v1: methods only → layouts unaffected).

   But WAIT: macro evaluation needs `self.fields` — fields are known after Phase A (Members with Kind=Field are added in BindUnitTypes? or DefineUnitMembers?). Driver calls only BindUnitTypes +
   ResolveUnitSignatures + ResolveStructLayouts... where's DefineUnitMembers called? Let me check TypeBinder.cpp quickly. Also need: are mixin members COPIED into the including class at bind time,
   or resolved via Includes chain at lookup? LookupMember walks Includes — so User's LookupMember("to_json") would find Serializable's macro def? No — macro def is NOT a method member; TypeBinder
   must skip MacroDef decls (or does it choke on them today? MacroDef inside mixin body today — does TypeBinder handle/skip it? Today macros are file-local text templates invoked via MacroInvoke;
   a `macro def` INSIDE a mixin is expanded only if invoked... the issue's `macro def` inside mixin is NEVER invoked explicitly — it's implicitly instantiated per including type. That's the new
   "real resolution" semantics.)

   So the seam step: for each type T (including classes/structs), for each mixin M in T's transitive includes, for each MacroDef D in M's body → evaluate D's body with SelfType = T → generated
   Method(s) spliced into T's Body + registered via AddMember(T, ...). Also for MacroDefs directly in T's body. Then remove/skip MacroDefs from binder member collection (binder must ignore
   MacroDef/MacroInvoke/MacroBlock decl kinds — check it does today).

   `macro do` blocks: evaluated per-unit — they don't add members; they run side effects (shell, puts, assert). Could stay per-unit pass at order 15 (parallel OK, const store OK, no registration)
   — but they may reference `__DIR__` and run shell: fine per-unit. BUT if macro do emits top-level decls (issue says "code generation hooks")... v1: macro do = pure compile-time side effects, no
   decl emission (list as open question). Actually simpler: macro do evaluated at the same seam step (serial, deterministic output ordering!) — compiler stdout interleaving from parallel units
   would be garbage; serial seam gives deterministic diagnostic output. RECOMMENDED: evaluate macro do at seam too. Hmm — but macro do in issue example uses `__DIR__` and `find` — no type info
   needed; seam is fine.

   Backtick constants `BUILD_COMMIT : String = `...`.trim`: top-level LocalDecl? How are global constants handled today (TopStmts with LocalDecl)? The CommandLit fold can happen per-unit at order
   15 (no store writes) — but determinism + the RAII seam reads lowered code... a StringLiteral is fine. Fold at order 15 per unit: OK. OR fold at seam. Decide: fold CommandLit wherever it appears
   (macro bodies evaluate them inline; outside macros, MacroExpansion pass sweeps Expr arena by index, evaluates CommandLit via ProcessRunner, replaces with StringLiteral — copy-out/write-back per
   ast-rewrite). `.trim`/`.lines` chains: fold via MacroOps when receiver is a StringLiteral Const — as part of the same sweep (pattern: Call(Member(CommandLit|folded, op))). Actually if
   CommandLit → StringLiteral first, then `.trim` is Call(Member(StringLiteral, "trim")) — folding that requires the MacroOps on const strings in the same pass — second mini-sweep or recursive
   fold at the CommandLit site (evaluate the whole member chain bottom-up since sub-exprs have smaller indices — the by-index sweep handles innermost first for free!).

   Then TypeChecker sees StringLiteral/ArrayLit of StringLiterals — ordinary typing via @[Literal] bindings. `.lines` → ArrayLit sugar → lowered by existing ArrayLit machinery in TypeChecker.
   Beautiful: zero new typing code.

   **ProcessRunner**: new `source/Volt/Core/Public/Volt/Core/Support/ProcessRunner.hpp` + Private impl. API:
   ```cpp
   struct ProcessResult { int ExitCode; std::string StdOut; std::string StdErr; bool bTimedOut; };
   VOLT_CORE_EXPORT ProcessResult RunProcess(std::string_view Command, std::string_view WorkDir, std::uint32_t TimeoutMs, std::size_t MaxOutputBytes);
   ```
   Shell execution: `/bin/sh -c cmd` POSIX (fork/exec + pipes + poll with timeout), `cmd.exe /c` Windows (CreateProcess) — but the project is Linux-first; multi-platform via #ifdef or std::system?
   std::system gives no capture. Use popen? popen captures stdout only, no timeout. For v1: POSIX fork/execvp("/bin/sh", "-c") + pipe + poll + timeout + output cap; Windows stub returning error
   diagnostic (listed as follow-up). Issue says "sécurisé et multi-plateforme" — design it with both backends, implement POSIX now (CI/dev are Linux), Windows behind the same interface with a
   clear TODO diag. Determinism/safety guardrails: max output (e.g. 1 MiB), timeout (e.g. 5s), non-zero exit → compile error with stderr captured. Working dir: the compiling file's directory
   (`__DIR__` semantics align).

   **Interpolation `@#{...}`**: parser support: after `At` token, if next is `#{`... lexer-wise `#{` is two tokens? `#` isn't a token currently. In strings, `#{` is recognized INSIDE string
   lexing. For `@#{`, parser sees `At` then... lexer would need to produce something. Let me check how the lexer handles string interpolation — need to read Lexer.cpp string handling. Also whether
   `@` followed by `#` lexes. Plan: parser rule: `At` + (InstanceVar token | `#{` sequence) — likely needs lexer support for a `HashInterp` start or lex `@#{` specially. Alternative cleaner: parse
   `@` then expect InstanceVar OR `#` `{` expr `}` — if lexer treats `#` as Error token outside strings today, add lexer support. Details for the parser task.

   **CommandLit lexing**: backtick `` ` `` currently unused (verify via grep). Lexer: on backtick, scan to matching backtick handling `#{...}` interpolation (same machinery as strings — check how
   strings do it: probably Lexer produces StringLiteral token per segment? Let me check Lexer.cpp + Parser Interp parsing to mirror it). Tokens: follow the string pattern — if strings use a single
   StringLiteral token + parser re-scan, do same; if they use Begin/Mid/End tokens, add CommandLiteralBegin/Mid/End. The issue's question explicitly asks which new ETokenKind — answer depends on
   existing string machinery; must check.

   **Hygiene**: `AstContext::MakeUniqueSymbol(Prefix)` exists (line 57) → generated locals (e.g. `json` accumulator if the engine synthesizes temps) use `__macro_N` prefix symbols; user code can't
   reference them (parser can't produce them? user COULD write __macro_0 — collision window; make prefix contain a character the lexer rejects in identifiers, e.g. `$`? Symbol is just interned
   text; parser identifiers can't contain `$` → prefix `__macro$` guarantees no collision. Nice trick — check Identifier lexing allows only [a-z_][a-z0-9_]*[?!].)

   Also ScopeResolver order 10 runs BEFORE MacroExpansion 15 today. If expansion moves to seam (before ALL passes), ScopeResolver sees expanded code — good. But ScopeResolver currently handles
   MacroDef/MacroInvoke (grep hit earlier) — check what it does (probably skips/metadata-marks them). If expansion moves to seam, MacroInvoke is gone before passes; ScopeResolver's macro arms
   become dead for expanded kinds but MacroDef decls may remain in mixins (they're per-type templates now — keep them in arena, binder skips them, AstInvariant doesn't check decls... fine).

   TypeBinder must SKIP MacroDef/MacroInvoke/MacroBlock when collecting members (check it already skips MacroDef/MacroInvoke today — it must, since they exist today; MacroBlock is new → add skip).

   **Cache (#61)**: stdlib units are cached post-parse+seam... on cache hit, seam SKIPS stdlib units (already bound). If a stdlib file had macro do/backticks, they'd run at cache-write time only →
   nondeterministic baked results + side effects lost. Guardrail: diag error when a unit marked for cache write contains MacroBlock or CommandLit outside macro bodies... simpler: stdlib =
   source/Lib files; ban `macro do` and backticks there via a check in the cache-write path or just document + SelfCheck test. User units aren't cached → fine. List as guardrail + open question.

   **Determinism/reproducible builds**: backticks make compilation impure. Mitigations: diag note suggesting pinning; cache invalidation N/A for user units (never cached); `volt check` runs them
   too (same pipeline). Sandboxing: none (it's a compiler feature like Rust build scripts / Crystal's `{{ run(...) }}`). Document.

   Now remaining checks:
   1. Lexer string/interp mechanics + backtick usage.
   2. TypeBinder: when Members filled; skips MacroDef?
   3. ScopeResolver's MacroDef/MacroInvoke arms.
   4. LiteralInferencer MetadataExprs macro marking.
   5. samples with macro + samples/Tests structure + tests golden.
   6. Skills: add-ast-node, add-sema-pass, format-and-check.
   7. How `getter` synthesizes methods (ConstructorSynthesis?) — to know how generated methods look / whether macro def can reuse.
   8. Check `LoweredSeamOrder()` value.
   9. MagicExpansion.cpp — how magics fold (for reusing MagicSite in macro eval).
   10. ParseStmt/ParseExpr: how `for`, `if`, string Interp are parsed (for macro body parsing — since macro def bodies become real parsed StmtLists, the parser already handles everything except
   `@#{` and backticks).
