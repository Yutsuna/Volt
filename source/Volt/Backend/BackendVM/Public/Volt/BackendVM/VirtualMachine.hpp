#pragma once

// VirtualMachine.hpp — the light stack VM behind `volt run` / `volt repl`.
//
// Skeleton: the execution engine is specified in .agents/backend/vm.md and
// lands with the `run` milestone. What is fixed now is the shape — value
// slots, activation frames, the function-table indirection hot reload
// patches — so the emitter and the loop can grow against a stable surface.

#include "BackendVM_export.hpp"
#include "Volt/BackendVM/Bytecode.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Volt
{

namespace Backend
{

    namespace VM
    {

        // A raw 64-bit slot. The middle-end typed every expression, so the
        // interpretation of a slot is static per instruction — no runtime tag
        // is needed for correctness, only for diagnostics.
        struct Value
        {

            std::uint64_t Raw = 0;
        };

        struct CallFrame
        {

            std::uint32_t Function = 0; // FunctionTable index
            std::uint32_t Ip       = 0; // byte offset into the chunk's Code
            std::uint32_t Base     = 0; // first stack slot of this frame
        };

        enum class ERunStatus : std::uint8_t
        {

            Ok            = 0,
            Halted        = 1,
            Raised        = 2,
            Unimplemented = 3,
        };

        class BACKENDVM_EXPORT VirtualMachine
        {

        public:

            // Execute `Entry` until Halt/Return. Skeleton: reports
            // Unimplemented until the dispatch loop lands.
            [[nodiscard]] ERunStatus Run ( const FunctionTable &Functions, std::size_t Entry );

        private:

            std::vector<Value> Stack;
            std::vector<CallFrame> Frames;
        };

    } // namespace VM

} // namespace Backend

} // namespace Volt
