#pragma once

// MonoDriver.hpp — the Monomorphizer queue and its drain.
//
// A generic member has no signature and no body until a call site fixes its
// arguments (the declare/define sweeps skip it outright). A call to one,
// discovered anywhere in EmitResolvedCall, enqueues a MonoRequest; this service
// is what turns each request into a defined llvm::Function, exactly the way
// DefineMember defines a concrete one, except the type of every expression and
// the resolution of every call comes from Sema::ReinstantiateBody's overlay
// rather than from the unit's own Values/Callees (Instantiate.hpp: a generic
// body's ExprIds are shared by every instantiation, so the unit's own
// single-slot-per-ExprId maps cannot hold more than one instantiation's answer
// at a time).
//
// Draining a body can itself discover further instantiations — a generic method
// calling another generic method — so the drain repeats until the queue is
// empty rather than running once.

#include "Volt/BackendCore/Monomorphizer.hpp"

#include "Core/EmitterServices.hpp"

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class MonoDriver
        {

        public:

            explicit MonoDriver ( EmitterServices &InServices ) : Services( &InServices )
            {
            }

            // Idempotent: MonoRequest::Key dedupes, so every call site reaching
            // the same instantiation is free to ask again.
            void Enqueue ( MonoRequest Request )
            {
                Queue.Enqueue( std::move( Request ) );
            }

            // Every request discovered so far, and everything a drained body
            // itself discovers, until the queue is empty.
            void Drain ();

            // The member a request names, plus the UnitView that declares it
            // (its Ast/Scopes are what ReinstantiateBody re-types the body
            // against). Null when the store declares no such member — a
            // middle-end contract violation, reported by the caller.
            [[nodiscard]] const Sema::Member *LookupMonoMember ( const MonoRequest &Request, const UnitView **OutUnit ) const;

            // One instantiation: resolve the member, get or declare its
            // Function (FunctionFor's own cache makes this idempotent),
            // reinstantiate the body under Sema::ReinstantiateBody's overlay,
            // and emit it exactly as DefineMember emits a concrete one.
            void EmitMonomorphizedBody ( const MonoRequest &Request );

        private:

            EmitterServices *Services = nullptr;
            Monomorphizer Queue;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
