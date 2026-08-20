#include "Volt/MiddleEnd/Analysis/Lifetime/Temporaries.hpp"

#include "MemberResolver.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/MiddleEnd/Analysis/Lifetime/CleanupRegion.hpp"
#include "Volt/MiddleEnd/Analysis/Lifetime/ExprOwnership.hpp"
#include "Volt/MiddleEnd/Analysis/Lifetime/FinalizeCallBuilder.hpp"
#include "Volt/MiddleEnd/Analysis/Raii/Ownership.hpp"
#include "Volt/MiddleEnd/Analysis/Raii/OwnershipInference.hpp"

#include <cstdint>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Volt::MiddleEnd::Analysis::Lifetime
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
        Volt::Core::Symbol Name;
        const MiddleEnd::Resolver::Binding *Bound = nullptr;
        MiddleEnd::TypeSystem::SemaTypeId Type;
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
        const MiddleEnd::TypeSystem::SemaTypeId Type = Context.Ctx.Values.ExprType( Id );

        // A callable this region *built* is not this region's to release.
        //
        // `ClosureLifting` already owns that environment's lifetime: it holds
        // the enclosing frame's own locals (every use of a captured variable
        // is rewritten, on both sides, into a load through it), and the region
        // that pass builds around the construction is what releases it —
        // `arr.each do |i| total = total + i end` reads `total` through that
        // env for the whole call. Releasing it here as well is a double free,
        // and releasing it *early* frees storage the enclosing scope keeps
        // reading.
        //
        // A callable this region merely *received* — the result of `f( 1 )` in
        // `f( 1 )( 2 )` — is a different value entirely: nobody built it here,
        // nobody else names it, and its environment dies with the expression.
        // The discriminator is therefore "did this site construct it", not
        // "is it callable"; the callable test only says which values the
        // question is worth asking about. Identified by the type claiming the
        // `FuncType` node kind, never by a Volt type name
        // (rules/zero-hardcode.md).
        if ( IsCallableType( Context, Type ) )
        {
            const Resolution *Built = ResolutionOf( Context, Id );
            if ( Built == nullptr or Built->bConstructs )
            {
                return false;
            }
        }

        return MiddleEnd::Analysis::Raii::IsFinalizeCandidateType( Context.Ctx.Types, Context.Ctx.Values, Type );
    }

    // --- Region discovery -------------------------------------------------

    bool ProcessStmtList ( TypeCheckerContext &Context, const Frontend::StmtList &Body, bool bHasTailValue = true );

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
                                std::vector<Frontend::ExprId> &Out,
                                std::unordered_set<std::uint32_t> &CalleeSites )
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

        // Branches and passthrough expressions propagate `bMoved` to their children:
        // when the expression itself is moved to the consumer, its branches/operands
        // produce that moved value and must not be finalized as temporaries in this region.
        if ( const auto *TernaryNode = std::get_if<Frontend::Ternary>( &Node ) )
        {
            if ( bMoved )
            {
                if ( TernaryNode->Then.IsValid() )
                {
                    MovedChildren.insert( TernaryNode->Then.Value );
                }
                if ( TernaryNode->Else.IsValid() )
                {
                    MovedChildren.insert( TernaryNode->Else.Value );
                }
            }
        }

        if ( const auto *TypedNode = std::get_if<Frontend::TypedExpr>( &Node ) )
        {
            if ( bMoved and TypedNode->Value.IsValid() )
            {
                MovedChildren.insert( TypedNode->Value.Value );
            }
        }

        if ( const auto *UpcastNode = std::get_if<Frontend::DynamicUpcast>( &Node ) )
        {
            if ( bMoved and UpcastNode->Value.IsValid() )
            {
                MovedChildren.insert( UpcastNode->Value.Value );
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
            for ( const Volt::Core::Symbol Name : ArgNames )
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
            return MiddleEnd::Analysis::Raii::ParameterEscapes( *Callee->Decl, Index );
        };

        if ( const auto *CallNode = std::get_if<Frontend::Call>( &Node ) )
        {
            // A callee is almost always a *designation* — a name, a member,
            // a naked type — and a designation owns nothing. Worse, asking:
            // `T.new( … )`'s callee is a `Member` carrying the construction's
            // own resolution, so it answers "owned" while the value it
            // describes belongs to the `Call` above it. Those stay `Moved`,
            // exactly as before.
            //
            // A callee that is itself a **call** is the one exception, and not
            // an exception to the rule but an instance of it: `f( 1 )( 2 )`
            // evaluates `f( 1 )` to a value, and that value is as much this
            // region's to release as any other unnamed one. It is recorded
            // rather than collected outright because it needs one thing an
            // ordinary site does not — the resolution keyed on that slot
            // belongs to the *parent* call (ExprCallEmitter.cpp looks the
            // callee's id up to emit the call), so it must stay put.
            if ( CallNode->Callee.IsValid() )
            {
                if ( std::holds_alternative<Frontend::Call>( Context.Ctx.Ast.Expr( CallNode->Callee ) ) )
                {
                    CalleeSites.insert( CallNode->Callee.Value );
                }
                else
                {
                    MovedChildren.insert( CallNode->Callee.Value );
                }
            }

            // A trailing block is handed to the callee, and the region
            // `ClosureLifting` built around this very call is what releases
            // its environment. Claiming it here too would release the same
            // buffer twice.
            if ( CallNode->BlockArg.IsValid() )
            {
                MovedChildren.insert( CallNode->BlockArg.Value );
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
                    Meta::ForEachField( Inner,
                                        [&] ( const char *, const auto &Field )
                                        {
                                            using F = std::remove_cvref_t<decltype( Field )>;
                                            if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                            {
                                                CollectOwnedSubExprs( Context, Field, MovedChildren.contains( Field.Value ),
                                                                      false, Out, CalleeSites );
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                            {
                                                for ( const Frontend::ExprId Child : Field )
                                                {
                                                    CollectOwnedSubExprs( Context, Child, MovedChildren.contains( Child.Value ),
                                                                          false, Out, CalleeSites );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                            {
                                                static_cast<void>( ProcessStmtList( Context, Field, /*bHasTailValue=*/true ) );
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
    // (`MiddleEnd::TypeSystem::ReinstantiateBody`) it is a fresh node recorded in
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
    [[nodiscard]] Frontend::ExprId
    PlaceInSlot ( TypeCheckerContext &Context, const Frontend::ExprId Slot, Frontend::ExprNode Content )
    {
        if ( Context.Redirects != nullptr )
        {
            const Frontend::ExprId NewId       = Context.Ctx.Ast.Add( std::move( Content ) );
            ( *Context.Redirects )[Slot.Value] = NewId;
            return NewId;
        }
        Context.Ctx.Ast.Expr( Slot ) = std::move( Content );
        return Slot;
    }

    [[nodiscard]] Volt::Core::SourceRange LocOf ( const Frontend::ExprNode &Node )
    {
        Volt::Core::SourceRange Loc;
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
    [[nodiscard]] Frontend::ExprId DetachSlot ( TypeCheckerContext &Context, const Frontend::ExprId Slot, const bool bIsCallee )
    {
        Frontend::AstContext &Ast = Context.Ctx.Ast;

        const MiddleEnd::TypeSystem::SemaTypeId Type = Context.Ctx.Values.ExprType( Slot );
        const Frontend::ExprId Moved                 = Ast.Add( Frontend::ExprNode{ Ast.Expr( Slot ) } );
        if ( Type.IsValid() )
        {
            Context.Ctx.Values.SetExprType( Moved, Type );
        }

        // A slot standing in a call's callee position is the *parent* call's
        // key, not its own: what sits at `CalleeResolution[Slot]` describes the
        // call being made *through* this value, and the parent still points
        // here. Moving it would leave the parent with no resolution at all,
        // which is exactly the "call at expression N carries no callee
        // resolution" this pass hit the first time it tried. The detached copy
        // needs nothing: its own resolution is keyed on its own callee, an id
        // this rewrite never touches.
        if ( bIsCallee )
        {
            return Moved;
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
                                               MiddleEnd::Resolver::ScopeId Scope,
                                               Volt::Core::SourceRange Loc,
                                               Frontend::ExprId Value,
                                               MiddleEnd::TypeSystem::SemaTypeId Type,
                                               Frontend::StmtList &Body )
    {
        Frontend::AstContext &Ast = Context.Ctx.Ast;

        const Volt::Core::Symbol Name = Ast.MakeUniqueSymbol( "__t" );

        const Frontend::ExprId TargetId = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Loc, .Name = Name } } );
        Context.Ctx.Scopes.Declare( Scope, Name, MiddleEnd::Resolver::BindingSite{ TargetId } );
        const MiddleEnd::Resolver::Binding *Bound = Context.Ctx.Scopes.Resolve( Scope, Name );
        if ( Bound != nullptr )
        {
            // A write, not a use — ScopeTable.hpp's own convention.
            Context.Ctx.Scopes.BindUse( TargetId, *Bound, false );
        }
        Context.Ctx.Values.SetExprType( TargetId, Type );
        Context.Ctx.Values.SetSiteType( MiddleEnd::Resolver::BindingSite{ TargetId }, Type );
        Context.LocalTypes[MiddleEnd::Resolver::BindingSite{ TargetId }] = Type;

        const Frontend::ExprId AssignId =
            Ast.Add( Frontend::ExprNode{ Frontend::Assign{ .Loc = Loc, .Target = TargetId, .Value = Value } } );
        Context.Ctx.Values.SetExprType( AssignId, Type );
        Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = AssignId } } ) );

        return Temporary{ .Name = Name, .Bound = Bound, .Type = Type };
    }

    // A fresh read of `Temp`. Value-AST nodes are single-owner, so every
    // occurrence needs its own node (rules/ast-value.md).
    [[nodiscard]] Frontend::ExprId
    ReadTemporary ( TypeCheckerContext &Context, const Temporary &Temp, Volt::Core::SourceRange Loc )
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
        std::unordered_set<std::uint32_t> CalleeSites;
        const bool bRootMoved = ( Use == ERootUse::Moved );
        CollectOwnedSubExprs( Context, Root, /*bMoved=*/bRootMoved, /*bIsRoot=*/true, Sites, CalleeSites );
        if ( MovedChild.IsValid() )
        {
            std::erase( Sites, MovedChild );
        }

        const bool bOwnRoot = Use == ERootUse::Discarded and IsOwnedTemporary( Context, Root );
        if ( Sites.empty() and not bOwnRoot )
        {
            return false;
        }

        Frontend::AstContext &Ast                        = Context.Ctx.Ast;
        const Volt::Core::SourceRange Loc                = LocOf( Ast.Expr( Root ) );
        const MiddleEnd::TypeSystem::SemaTypeId RootType = Context.Ctx.Values.ExprType( Root );

        // One scope for the whole region. Parent-less on purpose: a `__tN`
        // is never looked up by name, only ever reached through the
        // `Binding` captured at declaration.
        const MiddleEnd::Resolver::ScopeId RegionScope =
            Context.Ctx.Scopes.PushScope( MiddleEnd::Resolver::ScopeId{}, MiddleEnd::Resolver::EScopeKind::Branch );

        Frontend::StmtList RegionBody;
        std::vector<Temporary> Owned;
        Owned.reserve( Sites.size() + 1 );

        for ( const Frontend::ExprId Site : Sites )
        {
            const MiddleEnd::TypeSystem::SemaTypeId Type = Context.Ctx.Values.ExprType( Site );
            const Frontend::ExprId Value                 = DetachSlot( Context, Site, CalleeSites.contains( Site.Value ) );
            const Temporary Temp                         = DeclareTemporary( Context, RegionScope, Loc, Value, Type, RegionBody );

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
        const Frontend::ExprId Detached = DetachSlot( Context, Root, /*bIsCallee=*/false );
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
    bool ProcessStmtList ( TypeCheckerContext &Context, const Frontend::StmtList &Body, const bool bHasTailValue )
    {
        bool bChanged = false;

        for ( std::size_t Pos = 0; Pos < Body.Size(); ++Pos )
        {
            const Frontend::StmtId Id = Body[Pos];
            if ( not Id.IsValid() )
            {
                continue;
            }

            // **A body's last expression is that body's value only when the
            // body produces a value (`bHasTailValue`).** Volt has no `return`
            // keyword requirement: `def +( other ) -> String` ends on
            // `String.owned( buf, total )`, and that expression's result *is*
            // what the caller receives. When `bHasTailValue` is true, the tail
            // is `Moved` to the caller. When false (e.g. top-level unit init,
            // while loops, statement-position blocks), the tail is `Discarded`
            // and its temporary is finalized at the boundary.
            const bool bTail               = Pos + 1 == Body.Size();
            const bool bBranchHasTailValue = bTail and bHasTailValue;

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
                bChanged = ProcessStmtList( Context, WhileNode->Body, /*bHasTailValue=*/false ) or bChanged;
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
                bChanged = ProcessStmtList( Context, IfNode->Then, bBranchHasTailValue ) or bChanged;
                bChanged = ProcessStmtList( Context, IfNode->Else, bBranchHasTailValue ) or bChanged;
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
                    bChanged           = ProcessStmtList( Context, Clause.Body, bBranchHasTailValue ) or bChanged;
                }
                bChanged = ProcessStmtList( Context, CaseNode->ElseBody, bBranchHasTailValue ) or bChanged;
                continue;
            }
            if ( const auto *BeginNode = std::get_if<Frontend::BeginExpr>( &Inner ) )
            {
                bChanged = ProcessStmtList( Context, BeginNode->Body, bBranchHasTailValue ) or bChanged;
                for ( const Frontend::StmtId RescueId : BeginNode->RescueClauses )
                {
                    const auto &Rescue = std::get<Frontend::RescueClause>( Context.Ctx.Ast.Stmt( RescueId ) );
                    bChanged           = ProcessStmtList( Context, Rescue.Body, bBranchHasTailValue ) or bChanged;
                }
                bChanged = ProcessStmtList( Context, BeginNode->EnsureBody, /*bHasTailValue=*/false ) or bChanged;
                continue;
            }

            const bool bIsReturnedTail = bTail and bHasTailValue;
            const ERootUse Use         = bIsReturnedTail ? ERootUse::Moved : ERootUse::Discarded;
            bChanged                   = ProcessRoot( Context, ExprStmtNode->Expr, Use ) or bChanged;
        }

        return bChanged;
    }

} // namespace

bool RunTemporaries ( TypeCheckerContext &Context, const Frontend::StmtList &Body, const bool bHasTailValue )
{
    return ProcessStmtList( Context, Body, bHasTailValue );
}

} // namespace Volt::MiddleEnd::Analysis::Lifetime
