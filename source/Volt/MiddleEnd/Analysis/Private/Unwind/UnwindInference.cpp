// UnwindInference.cpp — the fixpoint behind `Member::bCanUnwind`.
//
// Two questions, one walk. A body arms the transport itself when it holds a
// `raise`, or a `break` with no loop of its own to leave — the two things
// StmtReturnBreakNext/RaiseEmitter turn into a store to `volt.exc.tag` /
// `volt.brk.flag`. A body arms it *by proxy* when it calls something that does.
// The first is local and settled in one pass; the second is the fixpoint.
//
// What the walk reads at a call site is the resolution, never the syntax: it
// asks `UnitCallees` about every expression id it visits and folds whatever
// comes back. That is deliberate — a call reaches the backend under three
// different keys (a `Call`'s callee sub-expression, a `Member` access standing
// in for a getter, a `Binary`/`Unary` whose operator resolved to a method), and
// re-deriving which node kinds those are would be this analysis growing its own
// opinion about the AST. Asking every id costs one hash lookup per node and
// cannot drift from what the emitter will actually do.
//
// An `abstract def` callee reads as "may unwind", full stop. The resolution
// names the contract; which body answers it is a fact about the receiver,
// settled per instantiation in the backend. Unioning over every concrete member
// of that spelling — the device `Raii::InferParameterEscape` is built on —
// would be sound *within one run*, and is what the refinement in
// `.agents/PROGRESS-issue-124-unwind.md` proposes; it is not sound across the
// stdlib frontend cache, which freezes a stdlib member's bit into an artifact
// that a later build reuses beside user types the union never saw. A user
// `Writer` whose `write` raises would then be called through a cached
// `write_string` stamped "cannot unwind", and the raise would be resumed past.
// The bit is only ever as good as the smallest scope it will be read in.

#include "Volt/MiddleEnd/Analysis/Unwind/UnwindInference.hpp"

#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Core/Support/CompilerSeams.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace Volt::MiddleEnd::Analysis::Unwind
{

namespace
{

    // One analysable body: the member to stamp, the unit AST its declaration
    // lives in, and that unit's resolutions. All three come from the same unit
    // ordinal, and a body with no resolutions is not analysable at all — an
    // unresolved call has to read as "may unwind", and a whole unit of them
    // would say nothing this fixpoint could use.
    struct Body
    {
        Member *Owner                      = nullptr;
        const Frontend::AstContext *Ast    = nullptr;
        const IR::UnitCallees *Resolutions = nullptr;
    };

    // `@[External( "libc", … )]` and friends: implemented outside Volt, so it
    // cannot raise a Volt exception and cannot take a Volt `break`. The same
    // reading `ExprResolvedCallEmitter` already applies when it decides to emit
    // no post-call check — a seam declared `@[External( "volt", … )]` is *not*
    // foreign, it is this build's own synthesised body.
    [[nodiscard]] bool IsForeign ( const TypeStore &Store, const Member &Decl )
    {
        if ( not Decl.ExternSymbol.IsValid() )
        {
            return false;
        }
        return not Decl.ExternLib.IsValid() or Store.Text( Decl.ExternLib ) != ::Volt::Core::CompilerSeams::Library;
    }

    class UnwindScan
    {

    public:

        UnwindScan ( const TypeStore &InStore, const Body &InSubject )
            : Store( InStore ), Ast( *InSubject.Ast ), Resolutions( *InSubject.Resolutions )
        {
        }

        [[nodiscard]] bool Run ( const Frontend::StmtList &Statements )
        {
            for ( const Frontend::StmtId Id : Statements )
            {
                WalkStmt( Id );
            }
            return bArms;
        }

    private:

        // Whatever the resolution recorded against `Id` says about unwinding —
        // nothing at all when `Id` is not a call site, which is most ids.
        //
        // Returns whether a *usable* resolution was found, which is not the
        // same as whether one was recorded: TypeChecker leaves partial entries
        // behind, and `EmitCall` reacts to one by looking under its other key
        // rather than by failing. Reading a partial entry as "unknown callee"
        // here would be this analysis inventing a call the backend never emits.
        bool FoldResolution ( Frontend::ExprId Id )
        {
            const IR::CalleeEntry *Entry = Resolutions.Get( Id );
            if ( Entry == nullptr or Entry->Decl == nullptr )
            {
                return false;
            }

            // An indirect callee is a closure the call site does not name, and
            // a dynamically dispatched one is chosen at runtime. Neither leads
            // to a `Member` this fixpoint could have stamped. A non-local
            // `break` reaches its caller through exactly the first of these, so
            // this is not a corner case — it is how `break` inside a `do … end`
            // keeps the check that consumes it.
            if ( Entry->bIndirect or Entry->bDynamicDispatch )
            {
                bArms = true;
                return true;
            }

            // Not a call at all: a resolution onto a `Field` is a load from the
            // receiver, which no more arms the transport than reading a local
            // does. `to_pointer`'s whole body is one of these.
            if ( Entry->Decl->Kind != EMemberKind::Method )
            {
                return true;
            }

            // Not a call: a machine conversion is a `ptrtoint`/`inttoptr`/cast
            // emitted in place, with no frame to return from.
            if ( Entry->MachineConversion != IR::EMachineConversion::None )
            {
                return true;
            }

            // A contract, not an implementation: the body that answers it is
            // chosen by the receiver, one instantiation at a time, and is not a
            // `Member` this fixpoint ever stamped.
            if ( Entry->Decl->bAbstract )
            {
                bArms = true;
                return true;
            }

            if ( IsForeign( Store, *Entry->Decl ) )
            {
                return true;
            }

            bArms = bArms or Entry->Decl->bCanUnwind;
            return true;
        }

        void WalkExpr ( Frontend::ExprId Id )
        {
            if ( bArms or not Id.IsValid() )
            {
                return;
            }

            const bool bResolvedHere = FoldResolution( Id );

            const Frontend::ExprNode &Expr = Ast.Expr( Id );
            std::visit(
                [&] ( const auto &Concrete )
                {
                    using T = std::decay_t<decltype( Concrete )>;
                    if constexpr ( std::is_same_v<T, Frontend::RaiseExpr> )
                    {
                        bArms = true;
                    }
                    else if constexpr ( std::is_same_v<T, Frontend::Call> )
                    {
                        // Both keys, because `EmitCall` tries both: the callee
                        // sub-expression first, the `Call` node itself as the
                        // fallback. A `Call` neither of them resolves is a call
                        // this analysis cannot account for, and the honest
                        // answer to one of those is "may unwind".
                        const bool bViaCallee = FoldResolution( Concrete.Callee );
                        if ( not bViaCallee and not bResolvedHere )
                        {
                            bArms = true;
                        }
                    }

                    WalkChildren( Concrete );
                },
                Expr );
        }

        void WalkStmt ( Frontend::StmtId Id )
        {
            if ( bArms or not Id.IsValid() )
            {
                return;
            }

            const Frontend::StmtNode &Stmt = Ast.Stmt( Id );
            std::visit(
                [&] ( const auto &Concrete )
                {
                    using T = std::decay_t<decltype( Concrete )>;
                    if constexpr ( std::is_same_v<T, Frontend::Break> )
                    {
                        // A `break` with a loop of this frame's own to leave is
                        // an ordinary branch to that loop's merge block. Only
                        // one with none — inside a closure body, leaving the
                        // *iterating* method — arms the transport
                        // (`EmitBreak`, StmtReturnBreakNext.cpp).
                        if ( Loops == 0 )
                        {
                            bArms = true;
                        }
                    }
                    else if constexpr ( std::is_same_v<T, Frontend::While> )
                    {
                        ++Loops;
                        WalkChildren( Concrete );
                        --Loops;
                        return;
                    }

                    WalkChildren( Concrete );
                },
                Stmt );
        }

        // The same reflected descent `InlineSummary` uses: every `ExprId` /
        // `StmtId` field of every node, whatever the node is. Nothing here
        // enumerates node kinds, so a new one is walked the day it is declared.
        template <typename NodeType> void WalkChildren ( const NodeType &Node )
        {
            if constexpr ( Meta::Reflected<NodeType> )
            {
                Meta::ForEachField( Node,
                                    [&] ( std::string_view, const auto &Field )
                                    {
                                        using FieldType = std::remove_cvref_t<decltype( Field )>;
                                        if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                                        {
                                            WalkExpr( Field );
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
                                        {
                                            for ( const Frontend::ExprId Child : Field )
                                            {
                                                WalkExpr( Child );
                                            }
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, Frontend::StmtId> )
                                        {
                                            WalkStmt( Field );
                                        }
                                        else if constexpr ( std::is_same_v<FieldType, Frontend::StmtList> )
                                        {
                                            for ( const Frontend::StmtId Child : Field )
                                            {
                                                WalkStmt( Child );
                                            }
                                        }
                                    } );
            }
        }

        const TypeStore &Store;
        const Frontend::AstContext &Ast;
        const IR::UnitCallees &Resolutions;
        std::uint32_t Loops = 0;
        bool bArms          = false;
    };

    // Every member in the store whose body this build can actually see, and
    // whose unit recorded resolutions to read it with.
    [[nodiscard]] std::vector<Body> CollectBodies ( TypeStore &Store,
                                                    std::span<const Frontend::AstContext *const> Units,
                                                    std::span<const IR::UnitCallees *const> Callees )
    {
        const auto Take = [&] ( Member &Entry, std::vector<Body> &Out )
        {
            if ( Entry.Kind != EMemberKind::Method or Entry.Unit >= Units.size() or Entry.Unit >= Callees.size() )
            {
                return;
            }
            if ( Units[Entry.Unit] == nullptr or Callees[Entry.Unit] == nullptr )
            {
                return;
            }
            Out.push_back( Body{ .Owner = &Entry, .Ast = Units[Entry.Unit], .Resolutions = Callees[Entry.Unit] } );
        };

        std::vector<Body> Bodies;
        for ( std::size_t TypeIdx = 0; TypeIdx < Store.TypeCount(); ++TypeIdx )
        {
            const NominalId Id{ static_cast<NominalId::ValueType>( TypeIdx ) };
            for ( Member &Entry : Store.MutableMembers( Id ) )
            {
                Take( Entry, Bodies );
            }
        }
        for ( Member &Entry : Store.MutableFreeFunctions() )
        {
            Take( Entry, Bodies );
        }
        return Bodies;
    }

    // The statement list to analyse, or nothing when there is no body to read.
    //
    // An `abstract def` and an `@[External]` declaration both have none, and
    // both must keep the store's `true`: the first is answered by a body chosen
    // per receiver, the second by a symbol outside this build.
    [[nodiscard]] const Frontend::Method *AnalysableMethod ( const Body &Subject )
    {
        const auto *MethodNode = std::get_if<Frontend::Method>( &Subject.Ast->Decl( Subject.Owner->Decl ) );
        if ( MethodNode == nullptr or MethodNode->bAbstract or MethodNode->bExternal )
        {
            return nullptr;
        }
        return MethodNode;
    }

} // namespace

void InferUnwindFreedom ( const std::span<const Frontend::AstContext *const> Units,
                          const std::span<const IR::UnitCallees *const> Callees,
                          TypeStore &Store )
{
    const std::vector<Body> Bodies = CollectBodies( Store, Units, Callees );

    // The optimistic start, and the only place a bit is ever cleared. A body
    // this build can read is presumed not to arm the transport; the walk below
    // is what contradicts it. Everything else — a cache-hit slot, an
    // `@[External]`, an `abstract def` — keeps whatever it already carries,
    // which for a fresh member is the safe `true`.
    //
    // Optimistic rather than pessimistic because of recursion: starting every
    // body at `true` would leave every cycle at `true` for ever, and a language
    // whose `each` is written in itself has a great many cycles.
    for ( const Body &Entry : Bodies )
    {
        if ( AnalysableMethod( Entry ) != nullptr )
        {
            Entry.Owner->bCanUnwind = false;
        }
    }

    // Monotone upward: a bit only ever goes false -> true, and every predicate
    // the scan reads is monotone in those bits, so the count of `true`s
    // strictly increases each round until it stops. Bounded by that count,
    // hence by `Bodies.size()`; the loop condition is the fixpoint itself, the
    // bound is belt-and-braces against a predicate that stops being monotone
    // one day — the same shape both RAII fixpoints use.
    for ( std::size_t Round = 0; Round <= Bodies.size(); ++Round )
    {
        bool bChanged = false;
        for ( const Body &Entry : Bodies )
        {
            if ( Entry.Owner->bCanUnwind )
            {
                continue;
            }
            const Frontend::Method *MethodNode = AnalysableMethod( Entry );
            if ( MethodNode == nullptr )
            {
                continue;
            }
            if ( UnwindScan{ Store, Entry }.Run( MethodNode->Body ) )
            {
                Entry.Owner->bCanUnwind = true;
                bChanged                = true;
            }
        }
        if ( not bChanged )
        {
            break;
        }
    }
}

} // namespace Volt::MiddleEnd::Analysis::Unwind
