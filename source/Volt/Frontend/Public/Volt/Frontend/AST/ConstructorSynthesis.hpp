#pragma once

#include "Frontend_export.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

namespace Volt::Frontend
{

// Synthesizes an empty default `def initialize` method on every `Class` and
// `Struct` that does not declare one explicitly in source.
//
// Runs in Driver::ParseOne right after parsing, before TypeBinder — ensuring
// every constructible type exposes an `initialize` member for `T.new` calls.
FRONTEND_EXPORT void SynthesizeDefaultConstructors ( AstContext &Context );

} // namespace Volt::Frontend
