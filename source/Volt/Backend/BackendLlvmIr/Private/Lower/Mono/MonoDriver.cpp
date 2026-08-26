// MonoDriver.cpp — this target's half of the drain.
//
// The loop is BackendCore's (MonoQueue::Drain): to a fixpoint, not once, since
// a drained body can itself enqueue further requests. What is bound here is the
// two target-specific answers it needs — when to stop, and how to emit one
// instantiation.

#include "Lower/Mono/MonoDriver.hpp"

#include "Volt/BackendCore/DiagnosticSink.hpp"

void Volt::Backend::Llvm::MonoDriver::Drain ()
{
    MonoQueue::Drain(
        Queue, [this] () { return Services->Diag->Failed(); },
        [this] ( const MonoRequest &Request ) { EmitMonomorphizedBody( Request ); } );
}
