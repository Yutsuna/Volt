#pragma once

// UnwindTransport.hpp — the Tier 1 unwind protocol every backend shares.
//
// Volt's exception transport is setjmp/sigsetjmp-free and personality-less:
// `raise` stores the exception into thread-local state and takes the
// *poisoned path* — an early return from the current frame. Every ordinary
// call is followed by a check of that state, so a `raise` several calls
// deep unwinds one `ret` at a time (backend/llvm.md).
//
// A non-local `break` inside a block reuses the same transport rather than
// inventing a second one: it sets `volt.brk.flag` (a separate i1 TLS,
// deliberately not folded into the exception tag — see llvm.md phase 6)
// and takes EmitPoisonedPath. Two post-call checks exist:
//
//   EmitUnwindCheck     tests tag OR brk.flag -> propagate. Used after
//                       every ordinary call and after `block.call(...)`.
//   EmitExceptionCheck  tests tag only -> propagate. Used at exactly one
//                       call site: the one that received the trailing
//                       `do...end` (bBlockBound). That call is what
//                       `break` terminates, so the flag is consumed (cleared)
//                       there rather than propagated.
//
// This header states the thread-local slot names and the protocol constants
// so every backend shares the same naming and a VM/WASM backend does not
// have to re-derive the transport layout.

#include "BackendCore_export.hpp"

#include <cstdint>
#include <string_view>

namespace Volt::Backend
{

struct BACKENDCORE_EXPORT UnwindTransport
{
    // Thread-local slot names — each backend creates these as TLS globals
    // (LLVM: llvm::GlobalVariable, thread_local; VM: thread-local index;
    // WASM: linear-memory offset per thread).
    static constexpr std::string_view ExceptionValueSlot = "volt.exc.value";
    static constexpr std::string_view ExceptionTagSlot   = "volt.exc.tag";
    static constexpr std::string_view BreakFlagSlot      = "volt.brk.flag";

    // The sentinel value in ExceptionTagSlot that means "nothing in flight".
    // Reuses NominalId::InvalidValue (0xFFFFFFFF) — the same sentinel
    // TypedId already uses for "no id".
    static constexpr std::uint32_t NoExceptionTag = 0xFFFFFFFF;
};

} // namespace Volt::Backend
