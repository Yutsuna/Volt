#pragma once

// WasmEncoder.hpp — a self-contained binary WebAssembly writer.
//
// No external toolchain: the backend emits the .wasm byte format directly
// (magic + version, then sections), exactly as specified in
// .agents/backend/wasm.md. LEB128 lives here because every integer in the
// format is one.

#include "BackendWASM_export.hpp"
#include "Volt/BackendCore/TargetBackend.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Backend
{

    namespace Wasm
    {

        // Section ids, from the WebAssembly binary format specification.
        enum class ESection : std::uint8_t
        {

            Type     = 1,
            Import   = 2,
            Function = 3,
            Memory   = 5,
            Export   = 7,
            Code     = 10,
            Data     = 11,
        };

        inline void WriteUnsignedLeb ( std::vector<std::uint8_t> &Out, std::uint64_t Value )
        {
            do
            {
                std::uint8_t Byte = Value & 0x7F;
                Value >>= 7;
                if ( Value != 0 )
                {
                    Byte |= 0x80;
                }
                Out.push_back( Byte );
            } while ( Value != 0 );
        }

        inline void WriteSignedLeb ( std::vector<std::uint8_t> &Out, std::int64_t Value )
        {
            bool bMore = true;
            while ( bMore )
            {
                std::uint8_t Byte = Value & 0x7F;
                Value >>= 7;
                const bool bSignClear = ( Byte & 0x40 ) == 0;
                if ( ( Value == 0 and bSignClear ) or ( Value == -1 and not bSignClear ) )
                {
                    bMore = false;
                }
                else
                {
                    Byte |= 0x80;
                }
                Out.push_back( Byte );
            }
        }

        // Accumulates sections and serialises them in id order behind the
        // 8-byte module header.
        class BACKENDWASM_EXPORT WasmModuleBuilder
        {

        public:

            // Append a completed section payload (the id and payload size are
            // written by Serialize).
            void AddSection ( ESection Id, std::vector<std::uint8_t> Payload );

            [[nodiscard]] std::vector<std::uint8_t> Serialize () const;

        private:

            struct Section
            {

                ESection Id;
                std::vector<std::uint8_t> Payload;
            };

            std::vector<Section> Sections;
        };

        class BACKENDWASM_EXPORT WasmBackend
        {

        public:

            [[nodiscard]] std::string_view Name () const
            {
                return "wasm";
            }

            void Begin ( const BackendInput &Input );

            [[nodiscard]] EEmitStatus EmitUnit ( const UnitView &Unit );

            [[nodiscard]] EmitResult Finalize ();

        private:

            const BackendInput *Build = nullptr;
            WasmModuleBuilder Builder;
        };

    } // namespace Wasm

} // namespace Backend

} // namespace Volt
