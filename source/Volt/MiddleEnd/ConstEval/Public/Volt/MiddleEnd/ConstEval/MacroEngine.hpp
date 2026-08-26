#pragma once

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Diagnostics/SourceManager.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndConstEval_export.hpp"

#include <span>

namespace Volt
{

namespace MiddleEnd::ConstEval
{

    /// Serial seam step: evaluate every `macro def` once per concrete target
    /// type, graft the Method it generated into that type's own Body and
    /// register it as one of the type's Members — after which the
    /// ResolveUnitSignatures loop that follows resolves its signature exactly
    /// as it would a method somebody wrote by hand. Runs the `macro do` blocks
    /// in the same step, in file order, and clears both node kinds out of the
    /// declaration lists so no later phase ever sees a macro.
    ///
    /// It has to be here, between SynthesizeFinalizeStubs and
    /// ResolveUnitSignatures, and not in a pass: a pass is per-unit and
    /// parallel and holds a `const TypeStore &`, while a generated method must
    /// become a *member* of a type that may live in another unit entirely. The
    /// store is frozen by the time the first pass runs, and both member lookup
    /// and LLVM emission iterate the store rather than the ASTs — so a Method
    /// added later would be resolvable by nobody and emitted by no-one, which
    /// is precisely the bug this whole step exists to end.
    ///
    /// Two consequences of sitting this early, both deliberate: a type's
    /// `Includes` are not filled yet, so the mixins a macro is inherited from
    /// are read off the AST (the way ParentNominals already does); and a
    /// field's type is not resolved yet, so introspection hands back the
    /// *spelling* the source wrote — which is what generated code wants anyway.
    ///
    /// A null entry in Units is a stdlib unit served from the frontend cache:
    /// already expanded when the cache was written, skipped here, exactly as
    /// SynthesizeFinalizeStubs skips it.
    VOLT_MIDDLEEND_CONSTEVAL_EXPORT void ExpandTypeMacros ( std::span<Frontend::AstContext *const> Units,
                                                            TypeSystem::TypeStore &Store,
                                                            const ::Volt::Core::SourceManager &Sources,
                                                            ::Volt::Core::DiagEngine::Bag &Diags,
                                                            std::span<const Frontend::AstContext *const> AllUnits = {} );

} // namespace MiddleEnd::ConstEval

} // namespace Volt
