#pragma once

#include "Frontend_export.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

namespace Volt
{

namespace Frontend
{

    // `name = (&.trim) >> (&.prefix("_"))` at module scope: a point-free
    // chain bound to a bare identifier has no expected-type slot anywhere
    // (unlike the same chain passed straight to `.map`/`|>`, which reads its
    // parameter type off the callee's own declared signature) — nothing
    // could ever type its synthesized parameter. Rewrites the statement into
    // an ordinary top-level generic `def name<T>( fn_tmp : T ) -> T ... end`
    // instead of a `Lambda`, so the name resolves and monomorphizes through
    // the same machinery `Array<T>#map<U>` already does — never a bespoke
    // "generic closure" mechanism.
    //
    // Runs from Driver::ParseOne, immediately after parsing and before
    // Sema::BindUnitTypes — the cross-unit seam that builds the free-function
    // table from Ast.TopDecls. A Method a Sema Lowering pass appends later
    // (FunctionalLowering runs at Order 8, long after that seam) would never
    // be found by name, which is exactly the gap this function closes by
    // running before the seam instead of inside the ordered pass list.
    FRONTEND_EXPORT void LowerPointFreeDefs ( AstContext &Context );

} // namespace Frontend

} // namespace Volt
