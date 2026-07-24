#pragma once

// TargetBackend.hpp — the compile-time contract every code generator
// satisfies, plus the one thin type-erased adapter the Driver uses to pick a
// target at runtime.
//
// The concept is the real interface: emitters are written against concrete
// types and dispatch over AST nodes with std::visit( Overloaded{ ... } ),
// so there is zero virtual call per node. The virtual hop exists only at
// unit granularity, where its cost is invisible.

#include "Volt/BackendCore/BackendInput.hpp"

#include <concepts>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace Volt
{

namespace Backend
{

    enum class EEmitStatus : std::uint8_t
    {

        Ok            = 0,
        Unimplemented = 1,
        Error         = 2,
    };

    // What one whole emission produced. Artifact naming stays a plain path:
    // an object file, a bytecode image, a .wasm module — the target decides.
    struct EmitResult
    {

        EEmitStatus Status = EEmitStatus::Unimplemented;
        std::string Artifact;
        std::string Message;
    };

    // A code generator: sees the whole build once, each unit once (in
    // circuit link order), then finalises into an artifact. Everything it
    // reads is settled — zero semantic analysis, zero type inference
    // (rules/core-ast.md is the input contract).
    template <typename T>
    concept TargetBackend = requires( T Generator, const BackendInput &Input, const UnitView &Unit ) {
        { Generator.Name() } -> std::convertible_to<std::string_view>;
        { Generator.Begin( Input ) } -> std::same_as<void>;
        { Generator.EmitUnit( Unit ) } -> std::same_as<EEmitStatus>;
        { Generator.Finalize() } -> std::same_as<EmitResult>;
    };

    // Runtime seam for `volt build --target ...`: one virtual hop per unit.
    class IBackend
    {

    public:

        virtual ~IBackend () = default;

        [[nodiscard]] virtual std::string_view Name () const  = 0;
        virtual void Begin ( const BackendInput &Input )      = 0;
        virtual EEmitStatus EmitUnit ( const UnitView &Unit ) = 0;
        virtual EmitResult Finalize ()                        = 0;
    };

    // Wraps any TargetBackend into the runtime seam without the emitter
    // itself ever declaring a virtual.
    template <TargetBackend T> class TBackendAdapter final : public IBackend
    {

    public:

        explicit TBackendAdapter ( T InImpl ) : Impl( std::move( InImpl ) )
        {
        }

        [[nodiscard]] std::string_view Name () const override
        {
            return Impl.Name();
        }

        void Begin ( const BackendInput &Input ) override
        {
            Impl.Begin( Input );
        }

        EEmitStatus EmitUnit ( const UnitView &Unit ) override
        {
            return Impl.EmitUnit( Unit );
        }

        EmitResult Finalize () override
        {
            return Impl.Finalize();
        }

    private:

        T Impl;
    };

    template <TargetBackend T> [[nodiscard]] std::unique_ptr<IBackend> MakeBackend ( T Impl )
    {
        return std::make_unique<TBackendAdapter<T>>( std::move( Impl ) );
    }

} // namespace Backend

} // namespace Volt
