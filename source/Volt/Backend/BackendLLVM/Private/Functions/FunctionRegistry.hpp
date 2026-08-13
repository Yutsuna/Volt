#pragma once

// FunctionRegistry.hpp — symbol -> llvm::Function*, and the sweeps that fill it.
//
// The registry proper is the cache: a mangled symbol is the cross-unit currency
// the linker uses too, so it — not a DeclId, which is only meaningful inside
// the arena that minted it — is what a declaration is keyed on.
//
// The sweeps below are free functions rather than members of anything: each one
// is a *pass over the build*, driven by the services bundle, and none of them
// owns state. Their implementations are one file apiece (DeclareSweep.cpp,
// DefineSweep.cpp, SynthesizedSweep.cpp, EntryPointEmitter.cpp,
// UnitInitEmitter.cpp) — they are declared together here because they are one
// module's worth of concern, not because they share an object.

#include "Core/EmitterServices.hpp"
#include "Core/LlvmFwd.hpp"
#include "Volt/MiddleEnd/TypeSystem/Instantiate.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class FunctionRegistry
        {

        public:

            explicit FunctionRegistry ( EmitterServices &InServices ) : Services( &InServices )
            {
            }

            // The linker symbol a resolved callee is reached by: the C spelling
            // verbatim for an `@[External]` member — the whole point of that
            // boundary — and the mangled scheme otherwise.
            [[nodiscard]] std::string SymbolOf ( const MiddleEnd::TypeSystem::Member &Entry,
                                                 MiddleEnd::TypeSystem::NominalId Owner,
                                                 std::span<const std::uint32_t> FlatArgs ) const;

            // The declaration for that symbol, created on demand: the declare
            // sweep covers every concrete member of the build, but a
            // monomorphised callee is only named once a call site fixes its
            // arguments.
            [[nodiscard]] llvm::Function *FunctionFor ( const MiddleEnd::TypeSystem::Member &Entry,
                                                        MiddleEnd::TypeSystem::NominalId Owner,
                                                        std::span<const std::uint32_t> FlatArgs );

            // One declaration. Returns null when the member is not something
            // codegen emits a symbol for (abstract, generic, a field), which is
            // not a failure.
            llvm::Function *DeclareMember ( const MiddleEnd::TypeSystem::Member &Entry, MiddleEnd::TypeSystem::NominalId Owner );

        private:

            EmitterServices *Services = nullptr;
            std::unordered_map<std::string, llvm::Function *> Functions;
        };

        // --- The sweeps ------------------------------------------------------

        // Create an llvm::Function for every concrete method and free function
        // of the build, before any body is emitted, so a call to a callee
        // declared in another unit — or later in this one — resolves with no
        // fixup pass.
        void DeclareAll ( EmitterServices &Services );

        // A `mixin`'s own concrete (non-`abstract`) methods are generic over
        // `self` in the exact sense a type parameter is: `Arithmetic#min`'s
        // `other : self` means "whichever type includes me", so it has no
        // signature — let alone a body — until an *including* type is the
        // receiver, the same way `Array<T>#push` has none until `T` is fixed.
        // Neither DeclareAll nor DefineAll may treat the mixin itself as that
        // receiver: `Store.Type( Id ).Params.Size() > 0` (the generic-owner
        // exclusion both sweeps already apply) does not catch it, since a mixin
        // commonly declares no generic parameters of its own. Detected by
        // reading the declaring unit's own AST node kind — the one fact the
        // TypeStore does not carry, since `Struct`/`Class`/`Mixin`/`Enum`
        // collapse to one `NominalType` shape at bind time.
        [[nodiscard]] bool IsMixinOwner ( const EmitterServices &Services, MiddleEnd::TypeSystem::NominalId Id );

        // Fill in the bodies every member of `Unit` declares.
        void DefineAll ( EmitterServices &Services, const UnitView &Unit );

        // One body. Silently skips a member with no body to emit (external,
        // abstract, generic); a member whose declaration is not the Method the
        // store says it is, is a contract violation and reported.
        void DefineMember ( EmitterServices &Services,
                            const MiddleEnd::TypeSystem::Member &Entry,
                            MiddleEnd::TypeSystem::NominalId Owner,
                            const UnitView &Unit );

        // Creates (but does not define) the llvm::Function for every entry in
        // Unit.Synth, up front — before any body in this unit is emitted, so a
        // FuncAddr in an ordinary member's own body (the original closure
        // literal's call site) always finds its target already registered in
        // SynthesizedFns, regardless of which body happens to be emitted first.
        void DeclareSynthesized ( EmitterServices &Services, const UnitView &Unit );
        void DeclareSynthesizedFn ( EmitterServices &Services,
                                    const MiddleEnd::IR::SynthesizedFunction &Fn,
                                    const UnitView &Unit,
                                    const MiddleEnd::TypeSystem::UnitTypes &Values );

        // Defines every entry's body. Split from DeclareSynthesized for the
        // reason above; unlike an ordinary member, nothing outside this unit
        // ever calls one, so there is no cross-unit ordering to keep beyond that.
        void DefineSynthesized ( EmitterServices &Services, const UnitView &Unit );
        // `Redirects` is null for an ordinary unit's own (concrete) closure —
        // ClosureLifting mutated its literal's slot in place, so there is
        // nothing to redirect — and non-null only when `Fn` came out of a
        // MiddleEnd::TypeSystem::ReinstantiateBody overlay (MonoBodyEmitter), where it is that
        // overlay's own ExprRedirectMap.
        void DefineSynthesizedFn ( EmitterServices &Services,
                                   const MiddleEnd::IR::SynthesizedFunction &Fn,
                                   const UnitView &Unit,
                                   const MiddleEnd::TypeSystem::UnitTypes &Values,
                                   const MiddleEnd::IR::UnitCallees &Callees,
                                   const MiddleEnd::IR::ExprRedirectMap *Redirects = nullptr );

        // Gives `_V_init_all` (declared, never defined, by the stdlib prelude's
        // `@[External( "volt", "_V_init_all" )]`) its body: every unit's
        // `_V_init_N`, in order, stopping early once one leaves an exception in
        // flight. Idempotent — a second call sees the function already defined
        // and returns immediately.
        [[nodiscard]] bool EmitInitAll ( EmitterServices &Services );

        // The C-linkage shim the runtime starts at, calling the Volt function
        // EmitOptions names as the entry. False only when it failed and the
        // diagnostic already says why.
        [[nodiscard]] bool EmitEntryPoint ( EmitterServices &Services );

        // Emit the synthetic module initialization function `_V_init_<Ordinal>`
        // which executes the unit's TopStmts in file order.
        void EmitUnitInit ( EmitterServices &Services, const UnitView &Unit );

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
