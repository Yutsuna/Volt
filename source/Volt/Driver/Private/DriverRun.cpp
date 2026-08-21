// DriverRun.cpp — Driver::Run(): `volt run`, through BackendJIT.
//
// The mirror of DriverBuild.cpp, and deliberately shaped like it: the same
// three-phase protocol over the same UnitViews, differing only in which backend
// the views are handed to. That symmetry is the visible payoff of splitting the
// emission layer out — nothing about *emitting* Volt appears twice.
//
// Everything backend-specific stays inside this TU's VOLT_ENABLE_JIT branch;
// Driver.hpp mentions no backend type, so a caller (RunCommand.cpp) never needs
// an LLVM header either.

#include "Volt/Driver/Driver.hpp"

#ifdef VOLT_ENABLE_JIT
    #include "Volt/BackendCore/TargetBackend.hpp"
    #include "Volt/BackendJIT/JitBackend.hpp"

    #include <cstdint>
    #include <utility>
    #include <vector>
#endif

#include <string>
#include <string_view>

Volt::Driver::RunResult Volt::Driver::Driver::Run ( const RunOptions &Options )
{
#ifndef VOLT_ENABLE_JIT
    static_cast<void>( Options );
    return RunResult{ .bOk     = false,
                      .Code    = 1,
                      .Message = "This build of volt was configured without the JIT (enable_jit=false); "
                                 "`volt run` is unavailable" };
#else
    if ( Options.Target != "native" )
    {
        return RunResult{
            .bOk = false, .Code = 1, .Message = "Unsupported --target '" + Options.Target + "': only 'native' runs" };
    }

    Backend::Jit::JitOptions JitOpts;
    JitOpts.Dylibs   = Options.Dylibs;
    JitOpts.OptLevel = Options.OptLevel;

    // Every unit is emitted, stdlib included. Skipping the leading units needs a
    // dylib that defines both their code *and* __volt_unwind_slots, so that
    // JIT-ed code and that dylib share one copy of the transport state
    // (UnwindTransport.hpp). Until the stdlib artifact carries the accessor,
    // emitting everything is the configuration that is actually correct.
    JitOpts.SkipUnitsBelow = 0;

    Backend::Jit::JitBackend JitImpl;
    JitImpl.SetOptions( std::move( JitOpts ) );

    const std::vector<Backend::UnitView> Views = MakeBackendViews();
    const Backend::BackendInput BackendIn{
        .Types = &MutableLayouts(), .Units = Views, .StdlibUnitCount = static_cast<std::uint32_t>( StdlibUnitCount() ) };

    JitImpl.Begin( BackendIn );

    // A unit that fails leaves the emission's own sink set, so every later
    // EmitUnit is a guarded no-op; Finalize() is what surfaces the message.
    for ( const Backend::UnitView &Unit : Views )
    {
        static_cast<void>( JitImpl.EmitUnit( Unit ) );
    }

    const Backend::EmitResult Emitted = JitImpl.Finalize();
    if ( Emitted.Status != Backend::EEmitStatus::Ok )
    {
        return RunResult{ .bOk = false, .Code = 1, .Message = "Finalize failed: " + Emitted.Message };
    }

    std::vector<std::string_view> Args;
    Args.reserve( Options.ProgramArgs.size() );
    for ( const std::string &Arg : Options.ProgramArgs )
    {
        Args.emplace_back( Arg );
    }

    const Backend::RunResult Ran = JitImpl.Run( Args );
    return RunResult{ .bOk = Ran.bOk, .Code = Ran.Code, .Message = Ran.Message };
#endif
}
