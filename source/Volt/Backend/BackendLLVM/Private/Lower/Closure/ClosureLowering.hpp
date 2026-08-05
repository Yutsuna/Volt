#pragma once

// ClosureLowering.hpp — the indirect call through a closure value, and the
// `next`-as-`ret` half of the non-local-exit protocol a lifted closure body
// shares with its call site.
//
// `Lambda`/`Block` are sugar (Nodes.inl): ClosureLifting rewrites every literal
// into a synthesized function plus `Proc.new( FuncAddr, env )` before
// TypeChecker finishes, so nothing here is closure-*literal* shaped — every
// entry point below operates on an already-resolved `{ code, env }` value.

#include "Core/EmitterServices.hpp"
#include "Core/LlvmFwd.hpp"

#include <span>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class BodyEmitter;

        class ClosureLowering
        {

        public:

            explicit ClosureLowering ( EmitterServices &InServices ) : Services( &InServices )
            {
            }

            // The closure *value*, uniform across the three backends (abi.md):
            // the two-slot `{ ptr code, ptr env }` aggregate — hence, like every
            // aggregate, an address.
            [[nodiscard]] llvm::StructType *ClosurePairType ();

            // `next [value]` inside a closure body with no loop of its own: the
            // block ends this invocation and hands `value` back, so it is a
            // `ret`. Kept here rather than in Stmt/ because it is the closure
            // protocol, not the loop one — the two only share a keyword.
            void EmitBlockNext ( BodyEmitter &Emitter, Frontend::ExprId Value );

            // An indirect call through a closure value: `f( x )` on a local
            // holding a callable, and `block.call( x )` on a `&block` parameter,
            // are the same emission — `CalleeEntry::bIndirect`, with the
            // signature Sema read off the receiver's own type arguments (result
            // first, then parameters).
            [[nodiscard]] llvm::Value *EmitIndirectCall ( BodyEmitter &Emitter,
                                                          Frontend::ExprId Id,
                                                          const Sema::CalleeEntry &Entry,
                                                          Frontend::ExprId Receiver,
                                                          std::span<const Frontend::ExprId> Args );

        private:

            EmitterServices *Services = nullptr;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
