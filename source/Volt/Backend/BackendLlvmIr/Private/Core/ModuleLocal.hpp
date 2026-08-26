#pragma once

// ModuleLocal.hpp — the same global, seen from the module being written now.
//
// Every cache in the emitter maps something the *build* knows (a mangled
// symbol, a unit-scoped binding, a vtable's name) to an `llvm::GlobalValue *`.
// In Whole granularity there is one module, so a cached pointer is always
// usable and these functions are the identity.
//
// In PerUnit granularity there are many, and an `llvm::Function *` belongs to
// exactly one of them: using a pointer from module A while writing module B
// produces IR that verifies as nonsense and crashes ORC. Rather than clear
// every cache at each module boundary — which would re-mangle, re-derive
// signatures, and lose the "is it defined yet" answer those caches carry — the
// cached pointer is kept and *translated*: a declaration of the same name and
// type is materialised in the current module on first use.
//
// The invariant that makes this sound is that a definition is emitted in one
// module only (DefineAll visits a member in its own unit, the monomorphiser
// dedupes on Key, EmitInitAll and the entry point run once). Everywhere else
// gets a declaration, which is exactly what a translated copy is.

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        struct EmitterServices;

        // Null in, null out — every call site is on a path that already reports
        // its own failure, and a second diagnostic here would only bury it.
        [[nodiscard]] llvm::Function *LocalCopy ( EmitterServices &Services, llvm::Function *Fn );

        [[nodiscard]] llvm::GlobalVariable *LocalCopy ( EmitterServices &Services, llvm::GlobalVariable *Var );

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
