// MonoDriver.cpp — the drain loop.
//
// To a fixpoint, not once: a drained body can itself enqueue further requests
// (a generic method calling another generic method), and Enqueue dedupes on
// MonoRequest::Key, so the loop terminates exactly when no new instantiation was
// discovered.

#include "Lower/Mono/MonoDriver.hpp"

#include "Core/DiagnosticSink.hpp"

#include <optional>

void Volt::Backend::Llvm::MonoDriver::Drain ()
{
    while ( not Services->Diag->Failed() )
    {
        const std::optional<MonoRequest> Request = Queue.Next();
        if ( not Request.has_value() )
        {
            return;
        }
        EmitMonomorphizedBody( *Request );
    }
}
