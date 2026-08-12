#include "Temporaries.hpp"

#include "../MemberResolver.hpp"
#include "CleanupRegion.hpp"
#include "ExprOwnership.hpp"
#include "FinalizeCallBuilder.hpp"
#include "Raii/Ownership.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Raii/OwnershipInference.hpp"

#include <cstdint>
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

    // `ResolutionOf` / `ProducesOwnedValue` live in Lifetime/ExprOwnership.hpp
    // — shared with `ScopeCleanup`, which asks the identical question of a
    // local's initializer.

    // Owned *and* worth materializing: a value nobody would finalize anyway
    // (an `Int32`, a `Pointer<T>`) is left exactly where it is, so ordinary
    // arithmetic never grows a region.
    [[nodiscard]] bool IsOwnedTemporary ( TypeCheckerContext &Context, const Frontend::ExprId Id )
    {
        if ( not Id.IsValid() or not ProducesOwnedValue( Context, Id ) )
        {
            return false;
        }
        const Sema::SemaTypeId Type = Context.Ctx.Values.ExprType( Id );

        // A callable's environment is *not* a full-expression value. A
        // capturing closure's env holds the enclosing frame's own locals
        // (`ClosureLifting` rewrites every use of a captured variable, on
        // both sides, into a load through that env), so releasing it at the
        // end of the statement that built the closure frees storage the
        // enclosing scope keeps reading — `arr.each do |i| total = total + i
        // end` then reads `total` through freed memory.
        //
        // Identified by the type claiming the `FuncType` node kind, never by
        // a Volt type name (rules/zero-hardcode.md). Skipping it costs a
        // counted leak for a capture-free closure, which is the safe side of
        // this pass's standing arbitration; giving env its real lifetime (the
        // scope of what it captures) is scope-level work, not
        // full-expression work.
        if ( IsCallableType( Context, Type ) )
        {
            return false;
        }

        return Sema::Raii::IsFinalizeCandidateType( Context.Ctx.Types, Context.Ctx.Values, Type );
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
    void CollectOwnedSubExprs (
        TypeCheckerContext &Context, Frontend::ExprId Id, bool bMoved, bool bIsRoot, std::vector<Frontend::ExprId> &Out )
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

        // A closure literal's body is not part of this statement. It becomes a
        // function of its own, swept on its own account, and its parameters
        // exist only inside it — materializing one of its sub-expressions into
        // *this* region emits code in the enclosing function that reads a slot
        // only the closure has. Ordinarily the literal is already gone by now;
        // under a re-instantiation it is still standing, because lifting it
        // there records a redirect rather than overwriting the slot.
        if ( std::holds_alternative<Frontend::Lambda>( Node ) or std::holds_alternative<Frontend::Block>( Node ) )
        {
            return;
        }

        const bool bConstructing = [&]
        {
            const Resolution *Found = ResolutionOf( Context, Id );
            return Found != nullptr and Found->bConstructs;
        }();

        std::unordered_set<std::uint32_t> MovedChildren;

        // `raise e` hands `e` to the unwind mechanism, which carries it to
        // whichever `rescue` catches it — that frame releases it. It is a
        // move out of this region, exactly like a `return`, and finalizing it
        // here would free the in-flight exception under its own handler.
        if ( const auto *RaiseNode = std::get_if<Frontend::RaiseExpr>( &Node ) )
        {
            if ( RaiseNode->Exception.IsValid() )
            {
                MovedChildren.insert( RaiseNode->Exception.Value );
            }
        }

        // An assignment hands its value to the place it writes — a local, a
        // field, or the memory behind a `Deref` (which is how a closure's env
        // is filled). Whoever owns that place releases it; this region must
        // not. True at any depth, not only when the assignment happens to be
        // the statement's own root.
        if ( const auto *AssignNode = std::get_if<Frontend::Assign>( &Node ) )
        {
            if ( AssignNode->Value.IsValid() )
            {
                MovedChildren.insert( AssignNode->Value.Value );
            }
        }

        // Does the callee keep the value handed to positional argument
        // `Index`? Unknown callee, named arguments (which this positional
        // mapping cannot follow), and a construction all answer yes — the
        // side of the arbitration that leaks rather than corrupts.
        //
        // `CalleeSite` is the expression standing in the call's callee slot,
        // and it is consulted first because a *closure literal* there answers
        // the question far better than any declaration can: the one member a
        // callable has is the `FuncType` claimant's `abstract call`, which
        // declares no parameters at all, so `ParameterEscapes` would read
        // every argument of every indirect call as kept — and a desugared
        // composition (`g( f( x ) )`) would leak its intermediate by
        // construction. `LowerClosureLits` recorded the literal's own answer
        // under that slot's id before rewriting it.
        const auto ArgumentEscapes = [&] ( const Frontend::ExprId CalleeSite, const Resolution *Callee,
                                           const Frontend::SymbolList &ArgNames, std::size_t Index )
        {
            if ( bConstructing )
            {
                return true;
            }
            for ( const Core::Symbol Name : ArgNames )
            {
                if ( Name.IsValid() )
                {
                    return true;
                }
            }
            if ( CalleeSite.IsValid() )
            {
                if ( const auto Found = Context.ClosureParamEscapes.find( CalleeSite.Value );
                     Found != Context.ClosureParamEscapes.end() )
                {
                    return Index >= Found->second.Size() or Found->second[Index];
                }
            }
            if ( Callee == nullptr or Callee->Decl == nullptr )
            {
                return true;
            }
            return Sema::Raii::ParameterEscapes( *Callee->Decl, Index );
        };

        if ( const auto *CallNode = std::get_if<Frontend::Call>( &Node ) )
        {
            // A callee is a *designation*, not a value the statement owns.
            // Materializing it would also break the backend, which keys the
            // resolution on `Call::Callee`'s own id
            // (BackendLLVM/.../ExprCallEmitter.cpp) — detaching that slot
            // moves the entry out from under it. Descent continues, so a
            // receiver hidden inside the callee (`( a + b ).trim`) is still
            // collected on its own account.
            if ( CallNode->Callee.IsValid() )
            {
                MovedChildren.insert( CallNode->Callee.Value );
            }

            // An argument is this region's to release only when the callee
            // was *proven* to borrow it (`Member::ParamEscapes`, derived at
            // the Driver seam by `Raii::InferParameterEscape`). Without that
            // proof `arr.push( s.dup )` is a double free rather than a leak:
            // the region releases the temporary, `Array<T>#push` stored it,
            // and the array's own element cascade releases it again.
            const Resolution *CallRes = ResolutionOf( Context, Id );
            for ( std::size_t Arg = 0; Arg < CallNode->Args.Size(); ++Arg )
            {
                const Frontend::ExprId Child = CallNode->Args[Arg];
                if ( Child.IsValid() and ArgumentEscapes( CallNode->Callee, CallRes, CallNode->ArgNames, Arg ) )
                {
                    MovedChildren.insert( Child.Value );
                }
            }
        }

        // An operator on a non-primitive receiver *is* a member call
        // (rules/core-ast.md's operator contract), so its right-hand side is
        // that member's first positional argument and answers to the same
        // question. The receiver never does: `( a + b ).trim` must still
        // release `a + b` once `trim` has returned.
        if ( const auto *BinaryNode = std::get_if<Frontend::Binary>( &Node ) )
        {
            if ( BinaryNode->Rhs.IsValid() and ArgumentEscapes( Frontend::ExprId{}, ResolutionOf( Context, Id ), {}, 0 ) )
            {
                MovedChildren.insert( BinaryNode->Rhs.Value );
            }
        }

        std::visit(
            [&] ( const auto &Inner )
            {
                using T = std::remove_cvref_t<decltype( Inner )>;
                if constexpr ( not std::is_same_v<T, std::monostate> )
                {
                    Meta::ForEachField(
                        Inner,
                        [&] ( const char *, const auto &Field )
                        {
                            using F = std::remove_cvref_t<decltype( Field )>;
                            if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                            {
                                CollectOwnedSubExprs( Context, Field, MovedChildren.contains( Field.Value ), false, Out );
                            }
                            else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                            {
                                for ( const Frontend::ExprId Child : Field )
                                {
                                    CollectOwnedSubExprs( Context, Child, MovedChildren.contains( Child.Value ), false, Out );
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

    // Puts `Content` where `Slot`'s parent will look for it, and returns the
    // id that ends up holding it.
    //
    // In an ordinary unit that is the slot itself. Under a re-instantiation
    // (`Sema::ReinstantiateBody`) it is a fresh node recorded in
    // `Context.Redirects`, because the slot belongs to a *generic* body: one
    // shared AST node, walked again by every other instantiation, so
    // overwriting it would give `Array<Int32>`'s regions to `Array<String>`.
    // Identical discipline, and identical reason, to `ClosureLifting`'s own
    // `RewriteSlot`.
    //
    // The returned id is the one every side table must be keyed against — a
    // scope binding on the *original* slot is invisible to a backend that
    // followed the redirect, which is exactly how a materialized temporary
    // once reached codegen with no binding at all.
    [[nodiscard]] Frontend::ExprId PlaceInSlot ( TypeCheckerContext &Context, const Frontend::ExprId Slot, Frontend::ExprNode Content )
    {
        if ( Context.Redirects != nullptr )
        {
            const Frontend::ExprId NewId         = Context.Ctx.Ast.Add( std::move( Content ) );
            ( *Context.Redirects )[Slot.Value] = NewId;
            return NewId;
        }
        Context.Ctx.Ast.Expr( Slot ) = std::move( Content );
        return Slot;
    }

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

        const Sema::SemaTypeId Type  = Context.Ctx.Values.ExprType( Slot );
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
    [[nodiscard]] Frontend::ExprId ReadTemporary ( TypeCheckerContext &Context, const Temporary &Temp, Core::SourceRange Loc )
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

        Frontend::AstContext &Ast       = Context.Ctx.Ast;
        const Core::SourceRange Loc     = LocOf( Ast.Expr( Root ) );
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
            const Sema::SemaTypeId Type  = Context.Ctx.Values.ExprType( Site );
            const Frontend::ExprId Value = DetachSlot( Context, Site );
            const Temporary Temp         = DeclareTemporary( Context, RegionScope, Loc, Value, Type, RegionBody );

            // The slot the parent still points at now reads the temporary —
            // which is what makes this a rewrite of one node rather than of
            // the whole tree above it.
            const Frontend::ExprId Read =
                PlaceInSlot( Context, Site, Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = Temp.Name } } );
            if ( Temp.Bound != nullptr )
            {
                Context.Ctx.Scopes.BindUse( Read, *Temp.Bound, true );
            }
            Context.Ctx.Values.SetExprType( Read, Type );

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
            const Frontend::ExprId CallId =
                BuildFinalizeCallOnReceiver( Context, Loc, [&] { return ReadTemporary( Context, *It, Loc ); }, It->Type );
            CleanupBody.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } ) );
        }

        const Frontend::ExprId Region =
            EmitBoundary( Ast, Context.Ctx.Values, Loc, std::move( RegionBody ), std::move( CleanupBody ), RootType );
        const Frontend::ExprId Placed = PlaceInSlot( Context, Root, Frontend::ExprNode{ Ast.Expr( Region ) } );
        if ( RootType.IsValid() )
        {
            Context.Ctx.Values.SetExprType( Placed, RootType );
        }

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
