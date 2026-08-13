// ExprIndirectCallEmitter.cpp — the `bIndirect` arm of a resolved call.
//
// The emission itself is ClosureLowering's (Lower/Closure/IndirectCallEmitter.cpp):
// it is the `{ code, env }` calling protocol, and the VM and WASM backends will
// each want their own. What belongs to the *expression* layer, and lives here,
// is the one syntactic rule about it — a trailing `do ... end` block binds to a
// declared `&block` parameter, and a callable's type carries positional
// arguments only, so there is no slot for one to bind to.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/EmitterServices.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/Closure/ClosureLowering.hpp"

#include <string>

llvm::Value *Volt::Backend::Llvm::EmitIndirectDispatch ( BodyEmitter &Emitter,
                                                         Frontend::ExprId Id,
                                                         const MiddleEnd::IR::CalleeEntry &Entry,
                                                         Frontend::ExprId Receiver,
                                                         std::span<const Frontend::ExprId> Args,
                                                         Frontend::ExprId Block )
{
    if ( Block.IsValid() )
    {
        static_cast<void>( Emitter.Fail( "llvm: the call at expression " + std::to_string( Id.Value ) +
                                         " passes a trailing block to a callable, whose type carries positional "
                                         "arguments only" ) );
        return nullptr;
    }
    return Emitter.Services().Closures->EmitIndirectCall( Emitter, Id, Entry, Receiver, Args );
}
