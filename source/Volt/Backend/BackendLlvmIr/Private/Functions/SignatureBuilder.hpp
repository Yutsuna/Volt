#pragma once

// SignatureBuilder.hpp — a declared member's signature, as an llvm::FunctionType.
//
// Parameter order is abi.md's, once for every target: `self`, then the declared
// parameters. (`ptr %env` trails a *closure* body, which is emitted from a
// lifted Lambda, not from a declaration — see Lower/Closure/.)

#include "Core/EmitterServices.hpp"
#include "Core/LlvmFwd.hpp"

#include <cstdint>
#include <span>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class SignatureBuilder
        {

        public:

            explicit SignatureBuilder ( EmitterServices &InServices ) : Services( &InServices )
            {
            }

            // The signature of `Entry` as a member of `Owner`, instantiated for
            // `FlatArgs`. Null when some part of it has no resolved layout, with
            // the diagnostic already recorded.
            [[nodiscard]] llvm::FunctionType *FunctionTypeOf ( const MiddleEnd::TypeSystem::Member &Entry,
                                                               MiddleEnd::TypeSystem::NominalId Owner,
                                                               std::span<const std::uint32_t> FlatArgs );

            // The layout a *declared* signature type means for a member of
            // `Owner` instantiated at `FlatArgs` — `InstanceLayouts::OfSignature`,
            // except for `self` (`SigType::SelfParam`), which that function
            // refuses by design: its own refusal is for a *field* typed `self`
            // (there is no such instance to embed one by value), but
            // `other : self` / `-> self` on a method (Comparable#<,
            // Arithmetic#+, rules/zero-hardcode.md's own example) is the
            // ordinary, load-bearing case — the receiver's own layout, exactly
            // like the leading `self` parameter FunctionTypeOf already computes.
            [[nodiscard]] MiddleEnd::TypeSystem::LayoutId SignatureLayoutOf ( MiddleEnd::TypeSystem::TypeStore &Store,
                                                                              MiddleEnd::TypeSystem::SigTypeId Id,
                                                                              MiddleEnd::TypeSystem::NominalId Owner,
                                                                              std::span<const std::uint32_t> FlatArgs );

        private:

            EmitterServices *Services = nullptr;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
