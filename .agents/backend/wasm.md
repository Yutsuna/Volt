# Backend spec: WebAssembly — `volt build --target wasm`

Web applications and client-side JSX components. Module `BackendWASM`
(`source/Volt/Backend/BackendWASM/`), **zero external dependencies**: the
backend writes the `.wasm` binary format itself (`WasmEncoder.hpp` — LEB128
+ section builder), no Emscripten, no binaryen.

## Module shape

`WasmModuleBuilder` accumulates sections and serialises `\0asm` + version 1,
sections in ascending id order:

| Section | Content |
|---|---|
| Type (1) | one entry per distinct function signature, layouts mapped below |
| Import (2) | the JS bridge (below) + `memory` when host-provided |
| Function (3) / Code (10) | one entry per emitted `def` + monomorphised instantiation |
| Memory (5) | one linear memory, initial pages from static data size |
| Export (7) | the entry function, `memory`, and every `@[External]`-visible symbol |
| Data (11) | string literals and other static aggregates |

## Types: `LayoutNode` → wasm

Value types: `Primitive{ Bits <= 32, i* }` → `i32`; `{ 64, i*/ptr }` → `i64`
except **pointers into linear memory are `i32`** (wasm32); `f32`/`f64`
directly; `i1` → `i32`. `Aggregate` never crosses a function boundary as a
value: aggregates live in linear memory at `LayoutEngine` offsets and are
passed as an `i32` address — the same numbers as every other target
(`abi.md`), so the ABI is one spec, three encoders.

Operator emission follows the shared protocol (`core-interfaces.md`);
the spelling table maps to wasm opcodes (`i32.add`, `f64.mul`,
`i32.lt_s`/`lt_u` by `i`/`u` spelling, etc.). Control flow uses wasm's
structured `block`/`loop`/`br_if` — the core AST's `If`/`While`/`CaseExpr`
map 1:1 onto them without a relooper, precisely because no sugar survived
lowering. `RaiseExpr`/`BeginExpr` use the error-slot scheme of `llvm.md`
tier 1 (wasm EH proposal later, same seam). Closures: `ClosureEnvFrame` env
in linear memory, function references via a table + `call_indirect` — the
`{ fn index, env ptr }` pair is the same closure aggregate as everywhere.

Linear memory is bump-allocated from a stdlib `@[External]`-style allocator
compiled in, exactly like native: the backend calls a symbol, never invents
a runtime (`rules/zero-hardcode.md`).

## The JS runtime bridge

Everything the browser owns is an **import**, namespaced `volt`:

```
(import "volt" "jsx_create_element" (func ...))   ; Volt.JSX.create_element
(import "volt" "dom_set_property"   (func ...))
(import "volt" "dom_add_listener"   (func ...))
(import "volt" "schedule"           (func ...))   ; event-loop hook
```

A small hand-written `volt.js` glue instantiates the module, provides these
imports over the DOM, and decodes strings from linear memory (`{ data, size }`
aggregates read at their `LayoutEngine` offsets). Events call back through an
exported dispatch function carrying a closure table index — the same
`call_indirect` table used for closures, so a JS event handler *is* a Volt
closure with no special machinery.

## The JSX dependency, stated honestly

`JsxLowering` is complete and emits `Volt.JSX.create_element( tag, props,
children )` — **but no stdlib type declares that API**, so `.vlx` files lower
and then type as nothing (`rules/core-ast.md`, "known non-goals"). This
backend does not work around it: shipping JSX to wasm requires the stdlib
declaration first, which needs an element type and heterogeneous `children`
— the same sum-type wall as `T?`. Until then the wasm target compiles
non-JSX Volt only, and this paragraph is the tracking record.

## CLI

`volt build --target wasm` (`rules/cli-surface.md`): same Driver pipeline,
`WasmBackend` through the `IBackend` seam, `EmitResult::Artifact` = the
`.wasm` path, plus the `volt.js` glue copied alongside.
