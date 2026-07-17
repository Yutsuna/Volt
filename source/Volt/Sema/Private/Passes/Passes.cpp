// Passes.cpp — definitions of every pass named in PassList.inl.
//
// This phase ships the pass *plumbing*: ScopeResolver and TypeChecker are
// skeletons that establish the signature and ordering, while JsxLowering is
// already real (it delegates to the Frontend AST-to-AST rewrite). Filling in
// scope resolution and type checking is a ~10-line-per-rule job against
// PassContext — no wiring changes required.

#include "Volt/Frontend/AST/JsxLowering.hpp"
#include "Volt/Sema/Pass.hpp"

namespace Volt
{

namespace Sema
{

    // Order 10 — bind identifiers to their declarations. Skeleton for now:
    // it will populate a scope table on PassContext in the next phase.
    void ScopeResolver ( PassContext &Context )
    {
        static_cast<void>( Context );
    }

    // Order 20 — rewrite JSX nodes into `Volt::JSX` runtime calls. Real:
    // the tree must be JSX-free before type checking and codegen.
    void JsxLowering ( PassContext &Context )
    {
        static_cast<void>( Frontend::JsxLowering( Context.Ast ).Run() );
    }

    // Order 30 — resolve every type annotation to a MemoryLayout and check
    // assignability. Skeleton: the TypeStore is threaded through so the full
    // pass drops in without touching the Driver.
    void TypeChecker ( PassContext &Context )
    {
        static_cast<void>( Context );
    }

} // namespace Sema

} // namespace Volt
