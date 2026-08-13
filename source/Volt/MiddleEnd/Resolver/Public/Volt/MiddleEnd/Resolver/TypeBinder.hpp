#pragma once

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndResolver_export.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Volt
{

namespace MiddleEnd::Resolver
{

    using TypeSystem::TypeStore;

    VOLT_MIDDLEEND_RESOLVER_EXPORT std::size_t
    BindUnitTypes ( const Frontend::AstContext &Ast, std::uint32_t Unit, TypeStore &Store, Core::DiagEngine::Bag &Diags );

    VOLT_MIDDLEEND_RESOLVER_EXPORT std::size_t
    DefineUnitMembers ( const Frontend::AstContext &Ast, std::uint32_t Unit, TypeStore &Store, Core::DiagEngine::Bag &Diags );

    VOLT_MIDDLEEND_RESOLVER_EXPORT void VerifySuperTypes ( TypeStore &Store, Core::DiagEngine::Bag &Diags );

    VOLT_MIDDLEEND_RESOLVER_EXPORT std::size_t
    BindTypes ( const Frontend::AstContext &Ast, TypeStore &Store, Core::DiagEngine::Bag &Diags );

    VOLT_MIDDLEEND_RESOLVER_EXPORT std::size_t
    DefineMembers ( const Frontend::AstContext &Ast, TypeStore &Store, Core::DiagEngine::Bag &Diags );

    VOLT_MIDDLEEND_RESOLVER_EXPORT std::size_t
    ResolveUnitSignatures ( const Frontend::AstContext &Ast, std::uint32_t Unit, TypeStore &Store, Core::DiagEngine::Bag &Diags );

    VOLT_MIDDLEEND_RESOLVER_EXPORT void
    ResolveStructLayouts ( std::span<const Frontend::AstContext *const> Units, TypeStore &Store, Core::DiagEngine::Bag *Diags );

    VOLT_MIDDLEEND_RESOLVER_EXPORT void SynthesizeFinalizeStubs ( std::span<Frontend::AstContext *const> Units,
                                                                  TypeStore &Store );

} // namespace MiddleEnd::Resolver

} // namespace Volt
