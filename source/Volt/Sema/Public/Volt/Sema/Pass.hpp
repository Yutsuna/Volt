#pragma once

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace Volt
{

namespace Sema
{

    // Everything a pass is allowed to touch for one source file. One
    // PassContext per AstContext keeps a whole-circuit compile embarrassingly
    // parallel: passes never reach across files, and diagnostics land in a
    // thread-local Bag that the Driver merges once at the end.
    struct PassContext
    {

        Frontend::AstContext &Ast;
        TypeStore &Types;
        Core::DiagEngine::Bag &Diags;
    };

    // A pass is a pure function over a PassContext. New pass = one line in
    // PassList.inl + one definition; the registry and ordering come for free.
    using PassFn = void ( * )( PassContext & );

    struct PassInfo
    {

        std::string_view Name;
        int Order  = 0;
        PassFn Run = nullptr;
    };

    // Forward-declare every pass function straight from the manifest.
#define VOLT_PASS( Name, Order ) void Name( PassContext &Context );
#include "Volt/Sema/PassList.inl"

    // The manifest passes, sorted ascending by Order (built once, cached).
    [[nodiscard]] std::span<const PassInfo> PassRegistry ();

    // Run every registered pass over Context, in Order. Returns the number
    // of passes executed.
    std::size_t RunPasses ( PassContext &Context );

} // namespace Sema

} // namespace Volt
