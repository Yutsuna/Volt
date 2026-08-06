#pragma once

// DiagnosticSink.hpp — the backend's one failure channel.
//
// A backend never diagnoses Volt source (rules/core-ast.md): reaching any of
// these messages means the middle-end handed over something its own invariants
// say it cannot, so the message names the hole, not the user's program.
//
// First failure wins — later ones are almost always its consequences — and the
// symbol being emitted is appended once, here, rather than at the ~60 call
// sites that report. BodyEmitter is what sets and clears the current function
// name, for exactly the span of one body.

// EEmitStatus itself, not the whole emitter surface: this header is included
// by nearly every TU in the module, and TargetBackend.hpp is where the status
// enum is actually declared.
#include "Volt/BackendCore/TargetBackend.hpp"

#include <string>
#include <utility>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class DiagnosticSink
        {

        public:

            // Record a middle-end contract violation and yield an Error status.
            // Returns EEmitStatus::Error so a reporting site can `return Fail(…)`.
            EEmitStatus Fail ( std::string InMessage );

            // True once emission has failed, so a walk can unwind without every
            // step re-checking the same condition.
            [[nodiscard]] bool Failed () const noexcept
            {
                return Status == EEmitStatus::Error;
            }

            [[nodiscard]] const std::string &Message () const noexcept
            {
                return Text;
            }

            // The body being emitted, appended to whatever message comes next:
            // every one of these names a *middle-end* fact that is missing, and
            // the first question about any of them is "in which body". The name
            // is the mangled one, which is exactly owner + method.
            void SetCurrentFunctionName ( std::string Name )
            {
                CurrentFunction = std::move( Name );
            }

            void ClearCurrentFunctionName ()
            {
                CurrentFunction.clear();
            }

        private:

            EEmitStatus Status = EEmitStatus::Ok;
            std::string Text;
            std::string CurrentFunction;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
