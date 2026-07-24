#pragma once

// Bytecode.hpp — everything derived from the Bytecode.inl manifest, plus the
// containers one compiled function lives in.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Backend
{

    namespace VM
    {

        enum class EOpCode : std::uint8_t
        {
#define VOLT_OP( Name, OperandBytes ) Name,
#include "Volt/BackendVM/Bytecode.inl"
        };

        inline constexpr std::size_t OpCount = []
        {
            std::size_t Count = 0;
#define VOLT_OP( Name, OperandBytes ) ++Count;
#include "Volt/BackendVM/Bytecode.inl"
            return Count;
        }();

        inline constexpr std::array<std::string_view, OpCount> OpNames = {
#define VOLT_OP( Name, OperandBytes ) #Name,
#include "Volt/BackendVM/Bytecode.inl"
        };

        inline constexpr std::array<std::uint8_t, OpCount> OpOperandBytes = {
#define VOLT_OP( Name, OperandBytes ) OperandBytes,
#include "Volt/BackendVM/Bytecode.inl"
        };

        [[nodiscard]] constexpr std::string_view NameOf ( EOpCode Op )
        {
            return OpNames[static_cast<std::size_t>( Op )];
        }

        // One compiled function body: flat code plus its constant pool. VM
        // values are 64-bit slots; what a slot means is decided by the layout
        // that produced it, never by a Volt type name.
        struct Chunk
        {

            std::vector<std::uint8_t> Code;
            std::vector<std::uint64_t> Constants;
            std::uint16_t LocalSlots = 0;
        };

        // Calls go through this indirection on purpose: hot reload re-emits a
        // unit's chunks and re-points the entries atomically, so every existing
        // call site — including frames live on the stack at their next call —
        // picks up the new body without patching a single call instruction.
        struct FunctionTable
        {

            std::vector<Chunk> Chunks;

            void Patch ( std::size_t Index, Chunk Fresh )
            {
                if ( Index < Chunks.size() )
                {
                    Chunks[Index] = std::move( Fresh );
                }
            }
        };

    } // namespace VM

} // namespace Backend

} // namespace Volt
