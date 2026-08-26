// DiagnosticSink.cpp — see DiagnosticSink.hpp.

#include "Volt/BackendCore/DiagnosticSink.hpp"

#include <string>
#include <utility>

Volt::Backend::EEmitStatus Volt::Backend::DiagnosticSink::Fail ( std::string InMessage )
{
    if ( Status != EEmitStatus::Error )
    {
        Status = EEmitStatus::Error;
        Text   = std::move( InMessage );
        if ( not CurrentFunction.empty() )
        {
            Text += " (while emitting '" + CurrentFunction + "')";
        }
    }
    return EEmitStatus::Error;
}
