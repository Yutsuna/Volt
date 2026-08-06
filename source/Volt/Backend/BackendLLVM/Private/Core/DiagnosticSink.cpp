// DiagnosticSink.cpp — see DiagnosticSink.hpp.

#include "Core/DiagnosticSink.hpp"

#include <string>
#include <utility>

Volt::Backend::EEmitStatus Volt::Backend::Llvm::DiagnosticSink::Fail ( std::string InMessage )
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
