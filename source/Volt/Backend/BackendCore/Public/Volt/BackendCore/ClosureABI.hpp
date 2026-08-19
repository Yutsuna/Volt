#pragma once

// ClosureABI.hpp — the two-slot closure-value layout every backend shares.
//
// A callable value (Proc<R, P...>) is a `{ ptr code, ptr env }` aggregate
// in memory (abi.md). ClosureLifting builds this pair in the middle-end;
// every backend that invokes a `bIndirect` callee reads it. The slot
// indices are stated here once rather than as magic numbers in each
// backend's indirect-call emitter.

#include "BackendCore_export.hpp"

#include <cstdint>

namespace Volt::Backend
{

// The closure-value ABI: a two-slot aggregate { ptr code, ptr env }.
struct BACKENDCORE_EXPORT ClosureABI
{
    static constexpr std::uint32_t CodeSlot  = 0;
    static constexpr std::uint32_t EnvSlot   = 1;
    static constexpr std::uint32_t SlotCount = 2;
};

} // namespace Volt::Backend
