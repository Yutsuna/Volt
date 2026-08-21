// UnwindSlots.cpp — the host-side implementation of the transport accessor.
//
// `__volt_unwind_slots` is the non-TLS route to the three transport slots plus
// the in-flight object's buffer (UnwindTransport.hpp states the contract and
// why it exists). This is the provider used when the code being run was JIT-ed
// and there is no precompiled stdlib to own the slots instead: the compiler's
// own process holds them, and the JIT resolves the symbol against itself.
//
// Ordinary native code, so `thread_local` simply works here — which is the
// entire point. Nothing in this file is reachable from an ahead-of-time build:
// `volt build` emits the globals into the program and addresses them directly.

#include "Volt/BackendCore/UnwindTransport.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace
{

// Set once, before anything runs, from the size the emission measured. A
// program with no `raise` never touches the buffer at all, so the default only
// has to be valid, not useful.
std::size_t GRequestedStorage = 1;

struct ThreadSlots
{

    void *ExcValue       = nullptr;
    std::uint32_t ExcTag = Volt::Backend::UnwindTransport::NoExceptionTag;
    std::uint8_t BrkFlag = 0;

    // Heap rather than a fixed array: the width is a property of the program
    // being run, and it is not known when this translation unit is compiled.
    // Over-aligned so an object of any Volt layout can be copied into it.
    void *Storage = nullptr;

    void *Table[Volt::Backend::UnwindTransport::SlotTableEntries] = {};

    ThreadSlots ()
    {
        constexpr std::size_t Alignment = alignof( std::max_align_t );
        const std::size_t Bytes         = ( ( GRequestedStorage + Alignment - 1 ) / Alignment ) * Alignment;

        Storage = std::aligned_alloc( Alignment, Bytes );
        if ( Storage == nullptr )
        {
            throw std::bad_alloc();
        }

        Table[Volt::Backend::UnwindTransport::SlotTableValueIndex]   = &ExcValue;
        Table[Volt::Backend::UnwindTransport::SlotTableTagIndex]     = &ExcTag;
        Table[Volt::Backend::UnwindTransport::SlotTableBreakIndex]   = &BrkFlag;
        Table[Volt::Backend::UnwindTransport::SlotTableStorageIndex] = Storage;
    }

    ~ThreadSlots ()
    {
        std::free( Storage );
    }

    ThreadSlots ( const ThreadSlots & )           = delete;
    ThreadSlots &operator=( const ThreadSlots & ) = delete;
};

} // namespace

void Volt::Backend::SetUnwindStorageSize ( std::size_t Bytes )
{
    // Never shrinks: two runs in one process share the provider, and the
    // buffer already handed to a live thread must stay wide enough for
    // whichever program asked for the most.
    if ( Bytes > GRequestedStorage )
    {
        GRequestedStorage = Bytes;
    }
}

extern "C" BACKENDCORE_EXPORT void *__volt_unwind_slots ()
{
    static thread_local ThreadSlots Slots;
    return static_cast<void *>( Slots.Table );
}
