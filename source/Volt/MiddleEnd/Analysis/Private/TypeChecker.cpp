// TypeChecker.cpp — Order 30 pass: gives every expression a type, and
// resolves the members that types make available.

#include "DeclStmtWalker.hpp"
#include "LiteralInferencer.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "Volt/MiddleEnd/IR/CalleeMap.hpp"
#include "Volt/MiddleEnd/Lowering/LoweringPasses.hpp"

#include <cstddef>
#include <variant>

namespace
{

// `T?` parses, and then nothing at all happens to it: ResolveTypeExpr falls
// into its generic branch, finds no stdlib type claiming the `NilableType`
// node kind, and hands back an invalid type in silence. Every use of `T?` was
// therefore accepted and untyped.
//
// Nilable types need a sum-type model Volt does not have, so this is refused
// rather than implemented — but refused *out loud*. An arena sweep catches
// every occurrence with its own SourceRange, including the ones no
// declaration reaches.
void RejectNilableTypes ( Volt::MiddleEnd::Core::PassContext &Context )
{
    const std::size_t Count = Context.Ast.TypeCount();
    for ( std::size_t Index = 0; Index < Count; ++Index )
    {
        const Volt::Frontend::TypeId Id{ static_cast<Volt::Frontend::TypeId::ValueType>( Index ) };
        if ( std::holds_alternative<Volt::Frontend::NilableType>( Context.Ast.Type( Id ) ) )
        {
            Context.Diags.Error( Volt::Frontend::LocOf( Context.Ast.Type( Id ) ), "nilable types are not implemented" );
        }
    }
}

} // namespace

void Volt::MiddleEnd::Analysis::TypeChecker ( Core::PassContext &Context )
{
    // TypeCheckerPass still lives at its pre-migration location (Etape 8,
    // Analysis, not yet moved) — this pass's own top-level entry point had to
    // move already because Core::Pass.hpp/PassList.inl (Etape 2, already
    // migrated) forward-declare it as Volt::MiddleEnd::Analysis::TypeChecker.
    // A namespace alias, not a using-directive, because the body below
    // qualifies through the name (Analysis::Foo).
    namespace TypeCheckerPass = Volt::MiddleEnd::Analysis;

    RejectNilableTypes( Context );

    Analysis::TypeCheckerContext State{ Context, Analysis::MetadataExprs( Context.Ast ) };

    // Top-level statements first, same order ScopeResolver settled on: a file
    // is a module, its top-level locals are its globals, and a `def` anywhere
    // in the file can read them regardless of textual position.
    for ( const Frontend::StmtId Id : Context.Ast.TopStmts )
    {
        Analysis::WalkStmt( State, Id );
    }
    Analysis::WalkDecls( State, Context.Ast.TopDecls );

    // Only after every ConstrainExprType in the file has had its say: a
    // literal passed as a call argument is naturally inferred before its
    // parameter's type ever reaches it (CallType's own comment — "arguments
    // are bound before being checked"), so rewriting inline the moment either
    // path first settles a type bakes in whichever ran first. See
    // LiteralLowering.hpp.
    Lowering::LowerArrayLits( State );
    Lowering::LowerHashLits( State );
    Lowering::LowerStringLits( State );

    // Before LowerEnumCases, not after: a `case self when .Some(val)`
    // pattern is, post-CaseLowering, syntactically identical to a genuine
    // `EnumCase` construction call and would otherwise be misidentified as
    // one. Unlike the other Lower* sweeps this one does not skip deferred
    // (generic-body) expressions — see EnumCaseLowering.hpp.
    Lowering::LowerEnumPatterns( State );

    // Same "final type only" discipline, same reason: an `EnumCase`
    // construction reached as a call argument settles its type before the
    // parameter constrains it. See EnumCaseLowering.hpp.
    Lowering::LowerEnumCases( State );

    // No-capture only (Phase 3a, .agents/PLAN_CLOSURE_LOWERING.md): a
    // capturing closure is left as a Lambda/Block until Phase 3b's env
    // rewrite lands, so this must run after the literal lowerings above but
    // still under the same "final type only" discipline.
    Lowering::LowerClosureLits( State );

    // Sixth post-walk sweep, same "final type only" discipline: a scope-local
    // whose type declares `finalize` gets an automatic call synthesized at
    // its method's fall-through exit (Phase 1 — see FinalizeLowering.hpp).
    // Must run before the snapshot loop below so its synthesized Calls'
    // CalleeResolution entries are captured into Context.Callees too.
    Lowering::InsertFinalizeCalls( State );

    // Snapshot the settled resolutions into the unit before the pass-local
    // state dies — inference refines entries in place, so only the final map
    // is the truth a backend may read (Layout/CalleeMap.hpp).
    if ( Context.Callees != nullptr )
    {
        for ( const auto &[Value, Found] : State.CalleeResolution )
        {
            Context.Callees->Set( Frontend::ExprId{ Value },
                                  Volt::MiddleEnd::IR::CalleeEntry{ .Decl             = Found.Decl,
                                                                    .Result           = Found.Result,
                                                                    .Params           = Found.Params,
                                                                    .BlockParam       = Found.BlockParam,
                                                                    .Bindings         = Found.Bindings,
                                                                    .Receiver         = Found.Receiver,
                                                                    .bConstructs      = Found.bConstructs,
                                                                    .bIndirect        = Found.bIndirect,
                                                                    .VTableSlot       = Found.VTableSlot,
                                                                    .bDynamicDispatch = Found.bDynamicDispatch } );
        }
    }
}
