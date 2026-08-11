#include "Temporaries.hpp"

#include "CleanupRegion.hpp"
#include "FinalizeCallBuilder.hpp"
#include "Raii/Ownership.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"

#include "Volt/Core/Diagnostics/SourceManager.hpp"

#include <cstdint>
#include <cstdlib>
#include <print>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Volt::Sema::TypeCheckerPass::Lifetime
{

namespace
{

    // What the region's own root expression is for.
    enum class ERootUse : std::uint8_t
    {
        // Its value goes somewhere that outlives the region — a local, the
        // caller, a field. The root is `Moved`, so the region must not
        // finalize it.
        Moved,
        // Its value is dropped the instant the region ends (a bare
        // expression statement, a loop condition). If the root is `Owned`,
        // the region owns it and must release it.
        Discarded,
    };

    // One materialized temporary: the name, the binding every occurrence of
    // it must share (codegen finds one stack slot through it — see
    // `FinalizeCallBuilder`'s `__fin_i`), and what it holds.
    struct Temporary
    {
        Core::Symbol Name;
        const Sema::Binding *Bound = nullptr;
        Sema::SemaTypeId Type;
    };

    // --- Ownership classification ----------------------------------------

    // Where a node's callee resolution is keyed. `Call` records against its
    // *callee*'s id; `Binary`/`Unary` record against their own, because
    // `MemberType` is handed the operator node itself (ExprInferencer.cpp).
    // Nothing else resolves a callee at all.
    [[nodiscard]] const Resolution *ResolutionOf ( TypeCheckerContext &Context, const Frontend::ExprId Id )
    {
        const Frontend::ExprNode &Node = Context.Ctx.Ast.Expr( Id );

        std::uint32_t Key = Id.Value;
        if ( const auto *CallNode = std::get_if<Frontend::Call>( &Node ) )
        {
            if ( not CallNode->Callee.IsValid() )
            {
                return nullptr;
            }
            Key = CallNode->Callee.Value;
        }
        else if ( not std::holds_alternative<Frontend::Binary>( Node ) and not std::holds_alternative<Frontend::Unary>( Node ) )
        {
            return nullptr;
        }

        const auto Found = Context.CalleeResolution.find( Key );
        return Found != Context.CalleeResolution.end() ? &Found->second : nullptr;
    }

    // Does evaluating `Id` *produce* a value whose release is now this
    // region's responsibility?
    //
    // Two proofs are accepted, and no third:
    //
    //   - `bConstructs` — `T.new( … )` allocates the storage it hands back.
    //     Certain by construction, no inference involved.
    //   - `Member::bReturnsOwned` — derived by the seam-time fixpoint in
    //     `Raii::InferReturnOwnership`, which read the callee's own body.
    //
    // A bare `Identifier`/`InstanceVar`/`Member`/`Deref` reads a place
    // somebody else owns and never reaches here at all, because it resolves
    // to no callee. That is the same reasoning `ScopeCleanup`'s alias-init
    // guard already applies to `a2 = a1`, one level down.
    [[nodiscard]] bool ProducesOwnedValue ( TypeCheckerContext &Context, const Frontend::ExprId Id )
    {
        const Resolution *Found = ResolutionOf( Context, Id );
        if ( Found == nullptr )
        {
            return false;
        }
        if ( Found->bConstructs )
        {
            return true;
        }
        // An indirect call goes through a callable *value*; the member it
        // resolves to is the `FuncType` claimant's abstract contract, which
        // has no body and so was never proven to return owned. Reading the
        // flag handles that without a special case.
        return Found->Decl != nullptr and Found->Decl->bReturnsOwned;
    }

    // Owned *and* worth materializing: a value nobody would finalize anyway
    // (an `Int32`, a `Pointer<T>`) is left exactly where it is, so ordinary
    // arithmetic never grows a region.
    [[nodiscard]] bool IsOwnedTemporary ( TypeCheckerContext &Context, const Frontend::ExprId Id )
    {
        if ( not Id.IsValid() or not ProducesOwnedValue( Context, Id ) )
        {
            return false;
        }
        return Sema::Raii::IsFinalizeCandidateType( Context.Ctx.Types, Context.Ctx.Values,
                                                    Context.Ctx.Values.ExprType( Id ) );
    }

    // --- Region discovery -------------------------------------------------

    bool ProcessStmtList ( TypeCheckerContext &Context, const Frontend::StmtList &Body );

    // Collects, in evaluation order (post-order — innermost first), every
    // proper sub-expression of `Root` this region must own.
    //
    // Two kinds of subtree are deliberately not collected from:
    //
    //   - the arguments of a **constructing** call. `Bag.new( arr )` stores
    //     its argument into a field of the object it builds, so the argument
    //     is `Moved`, and finalizing it here as well would free the same
    //     buffer twice. This is exactly the escape
    //     `ScopeCleanup::EscapesAsConstructorArgument` already refuses to
    //     finalize a *named* local for, applied to an unnamed one.
    //   - a nested `StmtList` (an `If` branch, a `while` body). Those are
    //     regions of their own, and are recursed into as such — a temporary
    //     created inside a loop body must be released once per iteration,
    //     not once per enclosing statement.
    void CollectOwnedSubExprs ( TypeCheckerContext &Context,
                                Frontend::ExprId Id,
                                bool bMoved,
                                bool bIsRoot,
                                std::vector<Frontend::ExprId> &Out )
    {
        if ( not Id.IsValid() )
        {
            return;
        }

        // Copied out: recursing into a nested StmtList below runs the whole
        // region transform, which calls `Add()` and may reallocate the Expr
        // arena (rules/ast-rewrite.md). Reading fields off a live reference
        // across that is the exact hazard that file documents.
        const Frontend::ExprNode Node = Context.Ctx.Ast.Expr( Id );

        const bool bConstructing = [&]
        {
            const Resolution *Found = ResolutionOf( Context, Id );
            return Found != nullptr and Found->bConstructs;
        }();

        std::unordered_set<std::uint32_t> MovedChildren;
        if ( bConstructing )
        {
            if ( const auto *CallNode = std::get_if<Frontend::Call>( &Node ) )
            {
                for ( const Frontend::ExprId Arg : CallNode->Args )
                {
                    if ( Arg.IsValid() )
                    {
                        MovedChildren.insert( Arg.Value );
                    }
                }
            }
        }

        std::visit(
            [&] ( const auto &Inner )
            {
                using T = std::remove_cvref_t<decltype( Inner )>;
                if constexpr ( not std::is_same_v<T, std::monostate> )
                {
                    Meta::ForEachField( Inner,
                                        [&] ( const char *, const auto &Field )
                                        {
                                            using F = std::remove_cvref_t<decltype( Field )>;
                                            if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                            {
                                                CollectOwnedSubExprs( Context, Field, MovedChildren.contains( Field.Value ),
                                                                      false, Out );
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                            {
                                                for ( const Frontend::ExprId Child : Field )
                                                {
                                                    CollectOwnedSubExprs( Context, Child,
                                                                          MovedChildren.contains( Child.Value ), false, Out );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                            {
                                                static_cast<void>( ProcessStmtList( Context, Field ) );
                                            }
                                        } );
                }
            },
            Node );

        if ( not bIsRoot and not bMoved and IsOwnedTemporary( Context, Id ) )
        {
            Out.push_back( Id );
        }
    }

    // --- Materialization --------------------------------------------------

    [[nodiscard]] Core::SourceRange LocOf ( const Frontend::ExprNode &Node )
    {
        Core::SourceRange Loc;
        std::visit(
            [&] ( const auto &Inner )
            {
                using T = std::remove_cvref_t<decltype( Inner )>;
                if constexpr ( not std::is_same_v<T, std::monostate> )
                {
                    Loc = Inner.Loc;
                }
            },
            Node );
        return Loc;
    }

    // Moves the node currently in `Slot` into a slot of its own, and carries
    // across the two side tables an expression's identity actually lives in:
    // its type, and — for an operator, whose resolution is keyed by its own
    // id rather than by a callee's — its callee resolution. The original key
    // is erased, because `Slot` is about to hold something else entirely and
    // a stale resolution on it would tell a backend to emit a call.
    [[nodiscard]] Frontend::ExprId DetachSlot ( TypeCheckerContext &Context, const Frontend::ExprId Slot )
    {
        Frontend::AstContext &Ast = Context.Ctx.Ast;

        const Sema::SemaTypeId Type = Context.Ctx.Values.ExprType( Slot );
        const Frontend::ExprId Moved = Ast.Add( Frontend::ExprNode{ Ast.Expr( Slot ) } );
        if ( Type.IsValid() )
        {
            Context.Ctx.Values.SetExprType( Moved, Type );
        }

        if ( const auto Found = Context.CalleeResolution.find( Slot.Value ); Found != Context.CalleeResolution.end() )
        {
            Context.CalleeResolution[Moved.Value] = Found->second;
            Context.CalleeResolution.erase( Slot.Value );
        }
        if ( Context.NakedTypeExprs.contains( Slot.Value ) )
        {
            Context.NakedTypeExprs.insert( Moved.Value );
        }
        return Moved;
    }

    // Declares `__tN` in `Scope` and returns both it and the statement that
    // fills it. Every later occurrence must `BindUse` the same `Binding`, or
    // codegen has no single slot to find (`SlotFor`, ExprPlaceEmitter.cpp) —
    // which is why the binding is carried on the `Temporary` rather than
    // re-resolved by name.
    [[nodiscard]] Temporary DeclareTemporary ( TypeCheckerContext &Context,
                                               Sema::ScopeId Scope,
                                               Core::SourceRange Loc,
                                               Frontend::ExprId Value,
                                               Sema::SemaTypeId Type,
                                               Frontend::StmtList &Body )
    {
        Frontend::AstContext &Ast = Context.Ctx.Ast;

        const Core::Symbol Name = Ast.MakeUniqueSymbol( "__t" );

        const Frontend::ExprId TargetId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = Name } } );
        Context.Ctx.Scopes.Declare( Scope, Name, Sema::BindingSite{ TargetId } );
        const Sema::Binding *Bound = Context.Ctx.Scopes.Resolve( Scope, Name );
        if ( Bound != nullptr )
        {
            // A write, not a use — ScopeTable.hpp's own convention.
            Context.Ctx.Scopes.BindUse( TargetId, *Bound, false );
        }
        Context.Ctx.Values.SetExprType( TargetId, Type );
        Context.Ctx.Values.SetSiteType( Sema::BindingSite{ TargetId }, Type );
        Context.LocalTypes[Sema::BindingSite{ TargetId }] = Type;

        const Frontend::ExprId AssignId =
            Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = Loc, .Target = TargetId, .Value = Value } } );
        Context.Ctx.Values.SetExprType( AssignId, Type );
        Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = AssignId } } ) );

        return Temporary{ .Name = Name, .Bound = Bound, .Type = Type };
    }

    // A fresh read of `Temp`. Value-AST nodes are single-owner, so every
    // occurrence needs its own node (rules/ast-value.md).
    [[nodiscard]] Frontend::ExprId
    ReadTemporary ( TypeCheckerContext &Context, const Temporary &Temp, Core::SourceRange Loc )
    {
        const Frontend::ExprId Id =
            Context.Ctx.Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = Temp.Name } } );
        if ( Temp.Bound != nullptr )
        {
            Context.Ctx.Scopes.BindUse( Id, *Temp.Bound, true );
        }
        Context.Ctx.Values.SetExprType( Id, Temp.Type );
        return Id;
    }

    // --- The region transform ---------------------------------------------

    // Turns the expression in `Root` into a full-expression region, if it
    // owns anything. One boundary, however many temporaries — the whole
    // point of the model.
    bool ProcessRoot ( TypeCheckerContext &Context, const Frontend::ExprId Root, const ERootUse Use )
    {
        if ( not Root.IsValid() )
        {
            return false;
        }

        // `x = <expr>` as a statement: the assignment's own value is handed
        // to the target, so it is Moved — but everything *under* it is still
        // this region's. Recognising the shape here rather than at the call
        // site keeps every caller passing a plain expression id.
        Frontend::ExprId MovedChild;
        if ( const auto *AssignNode = std::get_if<Frontend::Assign>( &Context.Ctx.Ast.Expr( Root ) ) )
        {
            MovedChild = AssignNode->Value;
        }

        std::vector<Frontend::ExprId> Sites;
        CollectOwnedSubExprs( Context, Root, /*bMoved=*/false, /*bIsRoot=*/true, Sites );
        if ( MovedChild.IsValid() )
        {
            std::erase( Sites, MovedChild );
        }

        const bool bOwnRoot = Use == ERootUse::Discarded and IsOwnedTemporary( Context, Root );
        if ( Sites.empty() and not bOwnRoot )
        {
            return false;
        }

        Frontend::AstContext &Ast   = Context.Ctx.Ast;
        const Core::SourceRange Loc = LocOf( Ast.Expr( Root ) );
        const Sema::SemaTypeId RootType = Context.Ctx.Values.ExprType( Root );

        // One scope for the whole region. Parent-less on purpose: a `__tN`
        // is never looked up by name, only ever reached through the
        // `Binding` captured at declaration.
        const Sema::ScopeId RegionScope = Context.Ctx.Scopes.PushScope( Sema::ScopeId{}, Sema::EScopeKind::Branch );

        Frontend::StmtList RegionBody;
        std::vector<Temporary> Owned;
        Owned.reserve( Sites.size() + 1 );

        for ( const Frontend::ExprId Site : Sites )
        {
            const Sema::SemaTypeId Type = Context.Ctx.Values.ExprType( Site );
            const Frontend::ExprId Value = DetachSlot( Context, Site );
            const Temporary Temp = DeclareTemporary( Context, RegionScope, Loc, Value, Type, RegionBody );

            // The slot the parent still points at now reads the temporary —
            // which is what makes this a rewrite of one node rather than of
            // the whole tree above it.
            Ast.Expr( Site ) = Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = Temp.Name } };
            if ( Temp.Bound != nullptr )
            {
                Context.Ctx.Scopes.BindUse( Site, *Temp.Bound, true );
            }
            Context.Ctx.Values.SetExprType( Site, Type );

            Owned.push_back( Temp );
        }

        // The root's own value has to become the region's tail. It is moved
        // to a slot of its own either way; when the root is itself owned and
        // discarded, it goes through a temporary first so the boundary has a
        // name to release.
        const Frontend::ExprId Detached = DetachSlot( Context, Root );
        Frontend::ExprId Tail           = Detached;
        if ( bOwnRoot )
        {
            const Temporary Temp = DeclareTemporary( Context, RegionScope, Loc, Detached, RootType, RegionBody );
            Owned.push_back( Temp );
            Tail = ReadTemporary( Context, Temp, Loc );
        }
        RegionBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = Tail } } ) );

        // Reverse creation order — last created, first released, the same
        // LIFO every other finalize ordering in this module produces.
        Frontend::StmtList CleanupBody;
        for ( auto It = Owned.rbegin(); It != Owned.rend(); ++It )
        {
            const Frontend::ExprId CallId = BuildFinalizeCallOnReceiver(
                Context, Loc, [&] { return ReadTemporary( Context, *It, Loc ); }, It->Type );
            CleanupBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } ) );
        }

        if ( std::getenv( "VOLT_RAII_TRACE" ) != nullptr and Context.Ctx.Sources != nullptr )
        {
            const Core::LineColumn Where = Context.Ctx.Sources->Resolve( Loc.File, Loc.Begin );
            std::println( stderr, "[raii] {}:{} temps={} ownroot={}", Context.Ctx.Sources->PathOf( Loc.File ), Where.Line,
                          Owned.size(), bOwnRoot );
        }

        EmitBoundaryInto( Ast, Context.Ctx.Values, Root, Loc, std::move( RegionBody ), std::move( CleanupBody ), RootType );

        Context.Ctx.Stats.RaiiTemporaries += Owned.size();
        Context.Ctx.Stats.RaiiOwnedCreated += Owned.size();
        Context.Ctx.Stats.RaiiFinalizes += Owned.size();
        Context.Ctx.Stats.RaiiCleanupPaths += 1;
        if ( MovedChild.IsValid() and IsOwnedTemporary( Context, MovedChild ) )
        {
            Context.Ctx.Stats.RaiiOwnedCreated += 1;
            Context.Ctx.Stats.RaiiMoves += 1;
        }
        return true;
    }

    // Walks one statement list, opening a region at every full-expression
    // root it contains and recursing into every nested body.
    //
    // "Full-expression root" is not a new concept the pass invents — it is
    // exactly the set of expression slots a statement evaluates to
    // completion before moving on: an initializer, a returned value, a
    // condition, a bare expression statement. A `while` condition is one of
    // them *in its own right*, not as part of the enclosing statement,
    // because it is re-evaluated every iteration; wrapping it any higher
    // would leak one temporary per turn of the loop.
    bool ProcessStmtList ( TypeCheckerContext &Context, const Frontend::StmtList &Body )
    {
        bool bChanged = false;

        for ( std::size_t Pos = 0; Pos < Body.Size(); ++Pos )
        {
            const Frontend::StmtId Id = Body[Pos];
            if ( not Id.IsValid() )
            {
                continue;
            }

            // **A body's last expression is that body's value.** Volt has no
            // `return` keyword requirement: `def +( other ) -> String` ends
            // on `String.owned( buf, total )`, and that expression's result
            // *is* what the caller receives. Classifying it as discarded —
            // which "a bare expression statement" otherwise means — makes
            // the region finalize the very buffer it is about to hand back,
            // and every caller of every stdlib string operator then reads
            // freed memory. Found exactly that way: `s = "aa" + "bb"`
            // aborting with "double free detected in tcache".
            //
            // The same holds one level down, for the tail of an `If`/`when`/
            // `begin` body standing in expression position — which is why
            // this is decided per StmtList rather than only for a method.
            const bool bTail = Pos + 1 == Body.Size();

            // Copied out before ProcessRoot's own Add()s
            // (rules/ast-rewrite.md). Nothing below writes the statement
            // back: every rewrite lands in an *expression* slot the
            // statement already points at.
            const Frontend::StmtNode Node = Context.Ctx.Ast.Stmt( Id );

            if ( const auto *Local = std::get_if<Frontend::LocalDecl>( &Node ) )
            {
                bChanged = ProcessRoot( Context, Local->Init, ERootUse::Moved ) or bChanged;
                continue;
            }
            if ( const auto *ReturnNode = std::get_if<Frontend::Return>( &Node ) )
            {
                bChanged = ProcessRoot( Context, ReturnNode->Value, ERootUse::Moved ) or bChanged;
                continue;
            }
            if ( const auto *WhileNode = std::get_if<Frontend::While>( &Node ) )
            {
                bChanged = ProcessRoot( Context, WhileNode->Cond, ERootUse::Discarded ) or bChanged;
                bChanged = ProcessStmtList( Context, WhileNode->Body ) or bChanged;
                continue;
            }

            const auto *ExprStmtNode = std::get_if<Frontend::ExprStmt>( &Node );
            if ( ExprStmtNode == nullptr )
            {
                continue;
            }

            // A statement-position control construct is not a region: its
            // *condition* is one, and each of its bodies is a set of them.
            const Frontend::ExprNode Inner = Context.Ctx.Ast.Expr( ExprStmtNode->Expr );
            if ( const auto *IfNode = std::get_if<Frontend::If>( &Inner ) )
            {
                bChanged = ProcessRoot( Context, IfNode->Cond, ERootUse::Discarded ) or bChanged;
                bChanged = ProcessStmtList( Context, IfNode->Then ) or bChanged;
                bChanged = ProcessStmtList( Context, IfNode->Else ) or bChanged;
                continue;
            }
            if ( const auto *CaseNode = std::get_if<Frontend::CaseExpr>( &Inner ) )
            {
                // The scrutinee is deliberately *not* opened as a region.
                // `CaseLowering` (order 22) has already rewritten every
                // clause pattern into a `Bool` expression that reads the
                // very same `Scrutinee` id, so wrapping that id in a
                // boundary would re-run the region — and its cleanup — once
                // per clause tested. An owned scrutinee is therefore a
                // counted leak here rather than a repeated free.
                for ( const Frontend::StmtId ClauseId : CaseNode->Clauses )
                {
                    const auto &Clause = std::get<Frontend::WhenClause>( Context.Ctx.Ast.Stmt( ClauseId ) );
                    bChanged           = ProcessStmtList( Context, Clause.Body ) or bChanged;
                }
                bChanged = ProcessStmtList( Context, CaseNode->ElseBody ) or bChanged;
                continue;
            }
            if ( const auto *BeginNode = std::get_if<Frontend::BeginExpr>( &Inner ) )
            {
                bChanged = ProcessStmtList( Context, BeginNode->Body ) or bChanged;
                for ( const Frontend::StmtId RescueId : BeginNode->RescueClauses )
                {
                    const auto &Rescue = std::get<Frontend::RescueClause>( Context.Ctx.Ast.Stmt( RescueId ) );
                    bChanged           = ProcessStmtList( Context, Rescue.Body ) or bChanged;
                }
                bChanged = ProcessStmtList( Context, BeginNode->EnsureBody ) or bChanged;
                continue;
            }

            bChanged = ProcessRoot( Context, ExprStmtNode->Expr, bTail ? ERootUse::Moved : ERootUse::Discarded ) or bChanged;
        }

        return bChanged;
    }

} // namespace

bool RunTemporaries ( TypeCheckerContext &Context, const Frontend::StmtList &Body )
{
    return ProcessStmtList( Context, Body );
}

} // namespace Volt::Sema::TypeCheckerPass::Lifetime
