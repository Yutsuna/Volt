#pragma once

// Transition alias, kept for the duration of the Sema -> MiddleEnd migration
// (REFACTO.md, Etape 11). Nothing in-tree includes it: every call site was
// requalified onto the module that actually owns the symbol
// (`MiddleEnd::TypeSystem::TypeStore`, `MiddleEnd::Core::RunPasses`, ...),
// which a flat namespace alias cannot express — `Volt::Sema::TypeStore` and
// `Volt::MiddleEnd::TypeStore` are not the same name. It exists so an
// out-of-tree consumer that only ever spelled the top-level namespace keeps
// compiling, and is deleted once none remain.
namespace Volt
{
namespace Sema = MiddleEnd;
} // namespace Volt
