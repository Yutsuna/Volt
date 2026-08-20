# MiddleEnd Specification: Macros (`macro def`, `macro do`, `` `cmd` ``)

A Volt macro is **not a template**. `macro def to_json` is an ordinary parsed
Volt body that the compiler *runs*, and what that run emits becomes the body of
a real method on the target type — declared, registered, resolved and emitted
exactly like one somebody wrote by hand. There is no second grammar, no `{% %}`,
no re-lexing of generated text, and nothing left in the arena that no pass
lowered.

Three constructs, one keyword:

| Construct | Written | Produces |
|---|---|---|
| `macro def name( params ) -> T` | in a type or a mixin body | a `Method` on every concrete type it applies to |
| `macro do ... end` | at file scope | nothing at all — it runs for its effects |
| `` `command` `` | anywhere | the command's stdout, as a literal |

---

## 1. Where expansion happens, and why it cannot happen anywhere else

`ConstEval::ExpandTypeMacros` runs at the **serial interface seam** in
`Driver.cpp`, immediately after `SynthesizeFinalizeStubs` and immediately before
the `ResolveUnitSignatures` loop.

That position is forced, not chosen:

- A macro in `mixin Serializable` generates a method for `class User`, which may
  live in **another unit**. Passes are per-unit and parallel and hold a
  `const TypeStore &` — they can neither write the store nor touch another
  unit's AST.
- `DeclareMembers` (TypeBinder phase A) freezes each type's member list at that
  same seam. Both name resolution (`LookupMemberOn`) and LLVM emission
  (`DeclareSweep.cpp`) iterate the **store**, not the ASTs — so a `Method` added
  by a later pass is resolvable by nobody and emitted by no one. That was the
  old macro engine's defining bug, and the reason issue #75 is titled *with real
  resolutions*.
- The precedent is already in the tree: `SynthesizeFinalizeStubs` grafts a
  synthesized `finalize` into a type's `Body` and registers it with
  `Store.AddMember` at this very seam. `ExpandTypeMacros` does the same thing,
  step for step.

Two consequences follow from sitting this early, and both are deliberate:

- `NominalType::Includes` is filled by the *signature* phase, which runs after
  this one — so the mixins a macro is inherited through are read **off the AST**,
  the way `ParentNominals` already reads the same links.
- `Member::Result` is not resolved yet — so `field.type` is the **spelling the
  source wrote**, not a resolved type. That is what generation wants anyway, and
  it keeps a Volt type name out of the C++ (`rules/zero-hardcode.md`).

The remaining half of compile-time evaluation — folding `` `cmd`.trim `` where no
macro is involved — stays an ordinary per-unit pass at order 15
(`MacroExpansion.cpp`): a linear sweep of the expression arena, parallel, with
nothing shared to write.

---

## 2. Comptime-Driven Staging

Nothing marks a statement as compile-time. Staging follows the **data**.

**R1 — Compile-time sources (closed list).** Introspection of the target type
(`self`, `self.fields`, `self.name`), a command literal (`` `uname` ``), a magic
constant (`__DIR__`, `__FILE__`, … — read from `MagicConstants.inl`, the same
manifest the order-16 pass reads), and a loop variable bound to one of those.

**R2 — Propagation.** A binding is compile-time when its initialiser *flows from
a source*; an expression is substitutable when its operands are compile-time and
its operation is in `MacroOps.inl`. A literal is a compile-time **value** but not
a **source** — which is exactly why `json = "{"` stays a runtime local and the
generated method builds its string at run time.

**R3 — Runtime.** `@ivar`, `self` in value position, the generated method's own
parameters, and any call whose receiver or callee is runtime.

**R4 — Control flow.** `if` / `case` over a condition the evaluator can answer
visits only the winning branch and is not emitted. `for x in <compile-time
sequence>` is unrolled once per element. A runtime condition or iterable keeps
its construct — an ordinary branch or loop inside a generated method is ordinary
code — and its body is still evaluated, so a loop nested in one still unrolls.
*There is no `For` node*: the parser desugars every `for` into
`seq.each { |x| … }` (`ParseStmt.cpp`), so that call shape is the only form
compile-time iteration can arrive in.

**R5 — Emission.** Anything not executed is emitted into the target type's arena,
with its compile-time parts substituted in place: an `Interp` whose parts are all
known becomes a `StringLiteral`, a `CommandLit` becomes its output, and
`@#{ expr }` becomes an ordinary `InstanceVar` whose interned lexeme keeps the
`@` (`MemberResolver.cpp` reads names that way).

**R6 — Compiler operations.** `MacroOps.inl` (`size`, `lines`, `trim`, `chomp`,
`basename`, `fields`, `name`, `type`) and `MacroCalls.inl` (`each`, `puts`) are
the compiler's own closed vocabulary. A call whose callee is one of them and
whose arguments are all compile-time is carried out during compilation and emits
nothing; that is the whole difference between `puts` (the compiler's console) and
`assert!( … )` (a call in the generated program).

**R7 — Hygiene.** The evaluator introduces no name of its own: compile-time
bindings vanish, and a runtime local keeps the name the source gave it. Anything
a lowering *does* synthesize goes through `AstContext::MakeUniqueSymbol`.

**R8 — Bounds.** Expansion depth is capped (32, a neighbour of
`MaxFinalizeDepth`), a macro body may run at most 256 commands, and a command
carries a 10 s timeout and a 1 MiB output cap. Every one of them fails with a
diagnostic; none of them hangs a build.

### Computable is not substitutable

The one subtlety worth stating on its own. Operators (`==`, `>`, `+`) are **not**
in `MacroOps.inl`, on purpose:

```volt
if `uname`.trim == "Linux"    # computable → the branch is chosen while compiling
  libc_linux_exit( code )
end

assert!( content.size > 0 )   # emitted → `assert!( 1024 > 0 )` in the binary
```

Both comparisons are answerable at compile time. The first is a **staging
decision**, so the evaluator answers it. The second is a value in a call
argument, so what gets substituted is only what R2 allows — `content.size`, an
operation of the manifest — and the comparison itself survives into the emitted
call. One rule, both behaviours.

---

## 3. What a macro body becomes

```volt
mixin Summable
  macro def total -> Int32
    sum = 0
    for field in self.fields
      sum += @#{field.name}
    end
    sum
  end
end

class Point
  include Summable
  getter x : Int32
  getter y : Int32
  getter z : Int32
end
```

`self.fields` is a source, so the loop unrolls; `sum = 0` is a literal, so it
stays a runtime local; `@#{field.name}` becomes each field's own ivar. `Point`
ends up with, in its own body and in the store's member table:

```volt
def total -> Int32
  sum = 0
  sum += @x
  sum += @y
  sum += @z
  sum
end
```

A macro that declares a return type ends on the method's result, so a
compile-time value in tail position materialises as its literal:
`macro def field_count -> Int32` whose body is `self.fields.size` generates a
method whose whole body is `3`.

---

## 4. Host commands

A backtick literal runs on the host **while compiling**, in the directory of the
file it is written in — the same directory `__DIR__` reports — through
`Core::RunShell` (`fork` + `execl /bin/sh -c`, two drained pipes, `poll` with a
deadline, `SIGKILL` on timeout). Its interpolations must themselves be
compile-time.

A non-zero exit status is a **compilation error** carrying the command's own
first line of stderr; so are a timeout, a truncated output and a failed spawn.
Nothing is ever silently empty.

This is allowed everywhere, `source/Lib` included. The consequence is stated here
rather than discovered later: **the frontend cache key hashes stdlib sources, not
command output**, so a command written in the stdlib is re-run only when a stdlib
source changes or the cache is refreshed (`--fresh-stdlib`). Do not put
`date`-like commands in library code and expect them to move.

Global constants take the same route, and are the ordinary way to reach build
metadata:

```volt
BUILD_COMMIT : String = `git rev-parse --short HEAD`.trim
```

---

## 5. Limits (v1)

| Limit | Why |
|---|---|
| `self.fields` is the type's **own** fields, in declaration order | inherited fields are not in the subclass's `Members`; extend when a real case asks |
| A `macro def` on a **generic** type is refused with a diagnostic | a field's written type mentions a parameter nothing can answer until instantiation, which is long after this seam |
| A macro that would redefine an existing member is refused | silently shadowing a hand-written method makes it unfindable in the source it appears to come from |
| A mixin is never itself a target | `self` means the concrete including type |
| `macro do` emits nothing; code in one that is not compile-time is reported | it would otherwise be silently dropped |

## 6. Where the code is

| Piece | File |
|---|---|
| Value model, manifest operations, command policy | `ConstEval/Private/MacroValue.{hpp,cpp}` |
| The evaluator (staging, unrolling, emission) | `ConstEval/Private/MacroEval.{hpp,cpp}` |
| The seam step (targets, grafting, registration, retiring) | `ConstEval/Private/MacroEngine.cpp` |
| Operation manifest | `ConstEval/Public/…/MacroOps.inl` |
| Call-shape manifest | `ConstEval/Public/…/MacroCalls.inl` |
| Order-15 fold of commands outside macros | `ConstEval/Private/MacroExpansion.cpp` |
| Host process runner | `Core/Private/Support/ProcessRunner.cpp` |
| Fixtures | `samples/Syntax/Macros/`, `samples/Tests/Macros/` |
