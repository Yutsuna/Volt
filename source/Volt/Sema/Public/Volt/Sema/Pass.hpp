#pragma once
#include "Sema_export.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"
namespace Volt::Sema
{
using PassContext = MiddleEnd::Core::PassContext;
using PassStats   = MiddleEnd::Core::PassStats;
using PassFn      = MiddleEnd::Core::PassFn;
using EPassKind   = MiddleEnd::Core::EPassKind;
using PassInfo    = MiddleEnd::Core::PassInfo;
using MiddleEnd::Core::LoweredSeamOrder;
using MiddleEnd::Core::PassRegistry;
using MiddleEnd::Core::RunPasses;
} // namespace Volt::Sema

// Pass.hpp (MiddleEnd/Core, DAG-neutral) forward-declares these entry points
// in their eventual `Volt::MiddleEnd::Lowering`/`Analysis` homes, but the
// Lowering/Analysis modules themselves don't exist yet (later migration
// steps) — the bodies still live here, under Sema/Private/Passes/, and Sema
// builds as its own shared library with hidden visibility by default. A
// redeclaration with SEMA_EXPORT is enough to make the symbol Sema already
// defines visible across the .so boundary PassRegistry.cpp links through;
// once Lowering/Analysis are real modules, these bodies (and this block)
// move with them and each gets its own module's export macro instead.
namespace Volt::MiddleEnd::Lowering
{
SEMA_EXPORT void FunctionalLowering ( Core::PassContext &Context );
SEMA_EXPORT void PipelineLowering ( Core::PassContext &Context );
SEMA_EXPORT void JsxLowering ( Core::PassContext &Context );
SEMA_EXPORT void CaseLowering ( Core::PassContext &Context );
SEMA_EXPORT void DotCallLowering ( Core::PassContext &Context );
SEMA_EXPORT void AssignLowering ( Core::PassContext &Context );
SEMA_EXPORT void IndexLowering ( Core::PassContext &Context );
SEMA_EXPORT void InterpLowering ( Core::PassContext &Context );
} // namespace Volt::MiddleEnd::Lowering

namespace Volt::MiddleEnd::Analysis
{
SEMA_EXPORT void TypeChecker ( Core::PassContext &Context );
SEMA_EXPORT void UnusedChecker ( Core::PassContext &Context );
SEMA_EXPORT void AstInvariant ( Core::PassContext &Context );
} // namespace Volt::MiddleEnd::Analysis
