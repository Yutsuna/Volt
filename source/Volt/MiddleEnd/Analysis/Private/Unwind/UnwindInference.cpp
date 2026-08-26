// UnwindInference.cpp — the fixpoint behind `Member::bCanUnwind`.
//
// Two questions, one walk. A body arms the transport itself when it holds a
// `raise`, or a `break` with no loop of its own to leave — the two things
// RaiseEmitter and StmtReturnBreakNext turn into a store to `volt.exc.tag` /
// `volt.brk.flag`. A body arms it *by proxy* when it calls something that does.
// The first is local and settled in one pass; the second is the fixpoint.
//
// What the walk reads at a call site is the resolution, never the syntax: it
// asks `UnitCallees` about every expression id it visits and folds whatever
// comes back. That is deliberate — a call reaches the backend under three
// different keys (a `Call`'s callee sub-expression, a `Member` access standing
// in for a getter, a `Binary`/`Unary` whose operator resolved to a method) — and
// re-deriving which node kinds those are would be this analysis growing its own
// opinion about the AST. Asking every id costs one hash lookup per node and
// cannot drift from what the emitter will actually do.
//
// --- Why it runs in two passes -------------------------------------------
//
// An `abstract def` callee is a contract; which body answers it is a fact about
// the *receiver*, settled per instantiation in the backend. Unioning over the
// concrete members of that spelling is sound — the callee is necessarily one of
// them — but only over the store the union was computed against, and a stdlib
// member's answer outlives that store: it is written into the frontend cache
// and read back by a build holding user types the union never saw. A `Writer`
// whose `write` raises, reached through a cached `write_string` stamped "cannot
// unwind", is a raise resumed past in silence.
//
// So the union is not what gets cached. Pass A computes the part that cannot
// change — does the body arm without going through a contract, and which
// contracts does it reach — and closes both transitively over concrete callees,
// so a consumer needs neither the declaring unit's AST nor its call graph.
// Pass B is the union, re-run in every build against that build's own store.
// The split is what makes the analysis composable with the cache at all.

#include "Volt/MiddleEnd/Analysis/Unwind/UnwindInference.hpp"

#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Core/Support/CompilerSeams.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace Volt::MiddleEnd::Analysis::Unwind
{

namespace
{

    // One analysable body: the member to stamp, the unit AST its declaration
    // lives in, and that unit's resolutions. All three come from the same unit
    // ordinal, and a body whose unit recorded no resolutions is not analysable
    // at all — every call in it would read as unaccounted-for, which says
    // nothing this fixpoint could use.
    struct Body
    {
        Member *Owner                      = nullptr;
        const Frontend::AstContext *Ast    = nullptr;
        const IR::UnitCallees *Resolutions = nullptr;
    };

    // What one body contributes before anything transitive is folded in.
    struct LocalFacts
    {
        // Arms the transport with no call of its own involved: a `raise`, a
        // non-local `break`, or a callee this analysis cannot name at all.
        bool bArms = false;
        // Concrete Volt callees, whose own facts fold into this member's.
        std::vector<const Member *> Concrete;
        // Contracts reached directly. Resolved against implementations only in
        // pass B, and only ever in the build doing the reading.
        std::vector<Symbol> Abstract;
    };

    // `@[External( "libc", … )]` and friends: implemented outside Volt, so it
    // can neither raise a Volt exception nor take a Volt `break`. The same
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

        [[nodiscard]] LocalFacts Run ( const Frontend::StmtList &Statements )
        {
            for ( const Frontend::StmtId Id : Statements )
            {
                WalkStmt( Id );
            }
            return std::move( Facts );
        }

    private:

        // Whatever the resolution recorded against `Id` says — nothing at all
        // when `Id` is not a call site, which is most ids.
        //
        // Returns whether a *usable* resolution was found, which is not the
        // same as whether one was recorded: TypeChecker leaves partial entries
        // behind, and `EmitCall` reacts to one by looking under its other key
        // rather than by failing. Reading a partial entry as "unknown callee"
        // would be this analysis inventing a call the backend never emits.
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
                Facts.bArms = true;
                return true;
            }

            // Not a call at all: a resolution onto a `Field` is a load from the
            // receiver, which no more arms the transport than reading a local
            // does. `String#to_pointer`'s whole body is one of these.
            if ( Entry->Decl->Kind != EMemberKind::Method )
            {
                return true;
            }

            // Nor is a machine conversion: a `ptrtoint` / `inttoptr` / cast
            // emitted in place, with no frame to return from.
            if ( Entry->MachineConversion != IR::EMachineConversion::None )
            {
                return true;
            }

            if ( Entry->Decl->bAbstract )
            {
                Facts.Abstract.push_back( Entry->Decl->Name );
                return true;
            }

            if ( IsForeign( Store, *Entry->Decl ) )
            {
                return true;
            }

            Facts.Concrete.push_back( Entry->Decl );
            return true;
        }

        void WalkExpr ( Frontend::ExprId Id )
        {
            if ( not Id.IsValid() )
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
                        Facts.bArms = true;
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
                            Facts.bArms = true;
                        }
                    }

                    WalkChildren( Concrete );
                },
                Expr );
        }

        void WalkStmt ( Frontend::StmtId Id )
        {
            if ( not Id.IsValid() )
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
                        // *iterating* method — arms the transport (`EmitBreak`,
                        // StmtReturnBreakNext.cpp).
                        if ( Loops == 0 )
                        {
                            Facts.bArms = true;
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
        LocalFacts Facts;
        std::uint32_t Loops = 0;
    };

    // Every member this store holds, so both passes iterate it the same way and
    // neither can forget the free functions.
    template <typename Fn> void ForEachMember ( TypeStore &Store, Fn &&Visit )
    {
        for ( std::size_t TypeIdx = 0; TypeIdx < Store.TypeCount(); ++TypeIdx )
        {
            const NominalId Id{ static_cast<NominalId::ValueType>( TypeIdx ) };
            for ( Member &Entry : Store.MutableMembers( Id ) )
            {
                Visit( Entry );
            }
        }
        for ( Member &Entry : Store.MutableFreeFunctions() )
        {
            Visit( Entry );
        }
    }

    // Every member in the store whose body this build can actually see, and
    // whose unit recorded resolutions to read it with.
    [[nodiscard]] std::vector<Body> CollectBodies ( TypeStore &Store,
                                                    std::span<const Frontend::AstContext *const> Units,
                                                    std::span<const IR::UnitCallees *const> Callees )
    {
        std::vector<Body> Bodies;
        ForEachMember( Store,
                       [&] ( Member &Entry )
                       {
                           if ( Entry.Kind != EMemberKind::Method or Entry.Unit >= Units.size() or Entry.Unit >= Callees.size() )
                           {
                               return;
                           }
                           if ( Units[Entry.Unit] == nullptr or Callees[Entry.Unit] == nullptr )
                           {
                               return;
                           }
                           Bodies.push_back(
                               Body{ .Owner = &Entry, .Ast = Units[Entry.Unit], .Resolutions = Callees[Entry.Unit] } );
                       } );
        return Bodies;
    }

    // The statement list to analyse, or nothing when there is no body to read.
    //
    // An `abstract def` and an `@[External]` declaration both have none, and
    // both must keep the store's safe `true`: the first is answered by a body
    // chosen per receiver, the second by a symbol outside this build.
    [[nodiscard]] const Frontend::Method *AnalysableMethod ( const Body &Subject )
    {
        const auto *MethodNode = std::get_if<Frontend::Method>( &Subject.Ast->Decl( Subject.Owner->Decl ) );
        if ( MethodNode == nullptr or MethodNode->bAbstract or MethodNode->bExternal )
        {
            return nullptr;
        }
        return MethodNode;
    }

    [[nodiscard]] bool NoteContract ( Member &Into, Symbol Contract )
    {
        if ( std::ranges::find( Into.UnwindAbstractCalls, Contract ) != Into.UnwindAbstractCalls.end() )
        {
            return false;
        }
        Into.UnwindAbstractCalls.PushBack( Contract );
        return true;
    }

    // Pass A: the cacheable summary.
    //
    // `bUnwindLocal` and `UnwindAbstractCalls` both start from what one body
    // says on its own, then close transitively over concrete callees — so a
    // member ends up carrying every contract anything it can reach will
    // consult, and a consumer never has to walk that graph again. Monotone in
    // both directions it can move: the bit only goes false -> true, the set only
    // grows, so the closure terminates.
    void SummarizeBodies ( TypeStore &Store, const std::vector<Body> &Bodies )
    {
        std::vector<std::pair<Member *, LocalFacts>> Facts;
        Facts.reserve( Bodies.size() );

        for ( const Body &Entry : Bodies )
        {
            const Frontend::Method *MethodNode = AnalysableMethod( Entry );
            if ( MethodNode == nullptr )
            {
                continue;
            }
            Facts.emplace_back( Entry.Owner, UnwindScan{ Store, Entry }.Run( MethodNode->Body ) );
        }

        // The optimistic start, and the only place either fact is ever cleared.
        // A body this build can read is presumed to arm nothing and reach no
        // contract; its own scan is what contradicts that. Everything else — a
        // cache-hit slot, an `@[External]`, an `abstract def` — keeps whatever
        // it already carries, which for a fresh member is the safe `true`.
        //
        // Optimistic rather than pessimistic because of recursion: starting
        // every body at `true` would leave every cycle at `true` for ever, and
        // a language whose `each` is written in itself has a great many cycles.
        for ( auto &[Owner, Local] : Facts )
        {
            Owner->bUnwindLocal = Local.bArms;
            Owner->UnwindAbstractCalls.Clear();
            for ( const Symbol Contract : Local.Abstract )
            {
                static_cast<void>( NoteContract( *Owner, Contract ) );
            }
        }

        for ( std::size_t Round = 0; Round <= Facts.size(); ++Round )
        {
            bool bChanged = false;
            for ( auto &[Owner, Local] : Facts )
            {
                for ( const Member *Callee : Local.Concrete )
                {
                    if ( Callee->bUnwindLocal and not Owner->bUnwindLocal )
                    {
                        Owner->bUnwindLocal = true;
                        bChanged            = true;
                    }
                    for ( const Symbol Contract : Callee->UnwindAbstractCalls )
                    {
                        bChanged = NoteContract( *Owner, Contract ) or bChanged;
                    }
                }
            }
            if ( not bChanged )
            {
                break;
            }
        }
    }

    // Pass B's index: every concrete member that could answer to a spelling.
    //
    // Abstract members are excluded from the index while still being asked *of*
    // it — a contract has no body, so leaving it in would make every spelling it
    // names unprovable through itself. A foreign `@[External]` is excluded for
    // the opposite reason: it is a real implementation, and one that cannot arm
    // the transport at all.
    using NameIndex = std::unordered_map<std::string_view, std::vector<const Member *>>;

    [[nodiscard]] NameIndex BuildNameIndex ( TypeStore &Store )
    {
        NameIndex Index;
        ForEachMember( Store,
                       [&] ( Member &Entry )
                       {
                           if ( Entry.Kind == EMemberKind::Method and not Entry.bAbstract and not IsForeign( Store, Entry ) )
                           {
                               Index[Store.Text( Entry.Name )].push_back( &Entry );
                           }
                       } );
        return Index;
    }

    // Pass B: the verdict, over *this* build's store.
    //
    // Sound where a cached union would not be, and for one reason: the store it
    // reads is the store the backend is about to emit from, so every type the
    // program can reach an implementation through is already in it.
    void ResolveVerdicts ( TypeStore &Store )
    {
        const NameIndex Index = BuildNameIndex( Store );

        std::vector<Member *> Pending;
        ForEachMember( Store,
                       [&] ( Member &Entry )
                       {
                           Entry.bCanUnwind = Entry.bUnwindLocal;
                           if ( not Entry.bCanUnwind and not Entry.UnwindAbstractCalls.IsEmpty() )
                           {
                               Pending.push_back( &Entry );
                           }
                       } );

        const auto AnyImplementationArms = [&] ( Symbol Contract )
        {
            const auto It = Index.find( Store.Text( Contract ) );
            if ( It == Index.end() )
            {
                // A contract nothing in this build implements. It is still
                // reached through a receiver the backend will monomorphise, so
                // the honest answer is "unknown".
                return true;
            }
            return std::ranges::any_of( It->second, [] ( const Member *Impl ) { return Impl->bCanUnwind; } );
        };

        // Monotone upward, same shape as pass A and as both RAII fixpoints.
        for ( std::size_t Round = 0; Round <= Pending.size(); ++Round )
        {
            bool bChanged = false;
            for ( Member *Entry : Pending )
            {
                if ( Entry->bCanUnwind )
                {
                    continue;
                }
                if ( std::ranges::any_of( Entry->UnwindAbstractCalls, AnyImplementationArms ) )
                {
                    Entry->bCanUnwind = true;
                    bChanged          = true;
                }
            }
            if ( not bChanged )
            {
                break;
            }
        }
    }

} // namespace

void InferUnwindFreedom ( const std::span<const Frontend::AstContext *const> Units,
                          const std::span<const IR::UnitCallees *const> Callees,
                          TypeStore &Store )
{
    SummarizeBodies( Store, CollectBodies( Store, Units, Callees ) );
    ResolveVerdicts( Store );
}

} // namespace Volt::MiddleEnd::Analysis::Unwind
