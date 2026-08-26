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

#include <cstddef>
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

    // --- The non-TLS route to the same three slots ------------------------
    //
    // A target that cannot emit a thread-local relocation reaches the slots
    // through a call instead: `ptr __volt_unwind_slots()` yields the address
    // of the calling thread's slot block, and the three slots are `gep`s at
    // the offsets below. That is the JIT's situation — JITLink resolves TLS
    // relocations only with ELFNixPlatform plus the orc-rt archive, a
    // dependency whose version is not ours to pin (backend/jit.md, "TLS") —
    // and it is the only reason this second route exists. `volt build` keeps
    // addressing the globals directly.
    //
    // Named here, once, so no backend invents its own symbol
    // (rules/zero-hardcode.md). The implementation is ordinary native code,
    // where thread_local works.
    static constexpr std::string_view SlotAccessorSymbol = "__volt_unwind_slots";

    // What the accessor returns is a table of the three slots' *addresses* —
    // three pointers — not a block that is itself the storage. That
    // indirection is the whole point, and it is what makes the two routes
    // interchangeable:
    //
    // JIT-ed code and precompiled native code have to agree on *one* copy of
    // the transport state. A program running under the JIT calls into a stdlib
    // that was compiled ahead of time, and that stdlib addresses the three
    // globals directly, by TLS relocation, because it is ordinary native code.
    // If the accessor handed back storage of its own, a `raise` inside the
    // stdlib would publish into one set of slots while the JIT-ed caller
    // checked another, and every exception crossing that boundary would be
    // lost. Handing back addresses instead means whoever defines the accessor
    // also decides where the slots live, and everyone reaches the same three.
    //
    // So: the accessor's definition belongs with the slots it points at. The
    // stdlib shared object defines both. A --no-stdlib build has no stdlib to
    // define them, so the compiler process provides both instead, and the JIT
    // finds it among the process's own symbols.
    //
    // Indices rather than byte offsets, deliberately: an emitter geps into a
    // three-pointer struct and lets the target's own data layout place the
    // fields. Nothing here bakes in a pointer width, which matters because
    // BackendCore is shared with targets whose is not the host's (wasm32).
    static constexpr std::size_t SlotTableValueIndex = 0;
    static constexpr std::size_t SlotTableTagIndex   = 1;
    static constexpr std::size_t SlotTableBreakIndex = 2;

    // The in-flight object's storage is the fourth, for the same reason as the
    // other three: it is thread-local, so JIT-ed code cannot address it either.
    // Unlike them it is *sized*, and the size is a property of the program
    // being run, not of whoever implements the accessor — so a JIT tells its
    // provider how wide to make it (SetUnwindStorageSize) before running
    // anything.
    static constexpr std::size_t SlotTableStorageIndex = 3;

    static constexpr std::size_t SlotTableEntries = 4;
};

// Sets how wide a buffer the host-side accessor hands back as its storage
// entry, for the program about to be run. Call before running anything; it only
// ever grows, so two programs sharing one process both get a buffer wide enough.
BACKENDCORE_EXPORT void SetUnwindStorageSize ( std::size_t Bytes );

} // namespace Volt::Backend
