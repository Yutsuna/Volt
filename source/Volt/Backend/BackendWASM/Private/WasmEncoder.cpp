#include "Volt/BackendWASM/WasmEncoder.hpp"

#include <algorithm>

static_assert( Volt::Backend::TargetBackend<Volt::Backend::Wasm::WasmBackend> );

void Volt::Backend::Wasm::WasmModuleBuilder::AddSection ( ESection Id, std::vector<std::uint8_t> Payload )
{
    Sections.push_back( Section{ .Id = Id, .Payload = std::move( Payload ) } );
}

std::vector<std::uint8_t> Volt::Backend::Wasm::WasmModuleBuilder::Serialize () const
{
    // Magic "\0asm" + version 1, then each section as id / payload-size /
    // payload, in ascending id order as the format requires.
    std::vector<std::uint8_t> Out{ 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };

    std::vector<const Section *> Ordered;
    Ordered.reserve( Sections.size() );
    for ( const Section &Entry : Sections )
    {
        Ordered.push_back( &Entry );
    }
    std::ranges::stable_sort( Ordered, [] ( const Section *A, const Section *B ) { return A->Id < B->Id; } );

    for ( const Section *Entry : Ordered )
    {
        Out.push_back( static_cast<std::uint8_t>( Entry->Id ) );
        WriteUnsignedLeb( Out, Entry->Payload.size() );
        Out.insert( Out.end(), Entry->Payload.begin(), Entry->Payload.end() );
    }
    return Out;
}

void Volt::Backend::Wasm::WasmBackend::Begin ( const BackendInput &Input )
{
    Build   = &Input;
    Builder = WasmModuleBuilder{};
}

Volt::Backend::EEmitStatus Volt::Backend::Wasm::WasmBackend::EmitUnit ( [[maybe_unused]] const UnitView &Unit )
{
    return EEmitStatus::Unimplemented;
}

Volt::Backend::EmitResult Volt::Backend::Wasm::WasmBackend::Finalize ()
{
    return EmitResult{
        .Status = EEmitStatus::Unimplemented, .Artifact = {}, .Message = "wasm backend: emission not implemented yet" };
}
