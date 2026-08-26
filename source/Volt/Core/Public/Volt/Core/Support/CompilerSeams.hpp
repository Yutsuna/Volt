#pragma once

// CompilerSeams.hpp — the pseudo-library that means "the build defines this".
//
// A Volt declaration marked `@[External( "volt", "_V_init_all" )]` names a
// symbol no Volt body can provide, because it is a fact about the *build*: the
// list of every unit to initialise, the table of every `:symbol` written
// anywhere. The backend synthesises them at the end of emission.
//
// The name is stated once, here, so nothing re-spells it: the linker skips it
// when turning `@[External]` libraries into `-l` flags (it is not a real
// library), and an executable backend has to know that a unit declaring one of
// these can never be taken from a precompiled artifact — its copy of the seam
// was filled in for a *different* build.
//
// It lives in `Core` rather than beside those readers because it has a second
// one on the other side of the compiler: `Unwind::InferUnwindFreedom` must tell
// a seam from a genuine foreign symbol, since C cannot raise a Volt exception
// and a synthesised Volt body very much can. MiddleEnd cannot see BackendCore —
// the dependency runs the other way — so the one authority moved down to the
// module both of them already depend on.

#include <string_view>

namespace Volt::Core
{

struct CompilerSeams
{

    // The `@[External]` library name that means "synthesised by this build".
    static constexpr std::string_view Library = "volt";
};

} // namespace Volt::Core
