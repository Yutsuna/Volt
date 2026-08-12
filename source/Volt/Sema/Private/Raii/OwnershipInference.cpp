#include "Volt/Sema/Raii/OwnershipInference.hpp"

#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace Volt::Sema::Raii
{

namespace
{

    // `T.new( … )`. The spelling belongs to Volt, not to C++ — but so does
    // every other member spelling the compiler looks up by name, and this one
    // is already the compiler's own vocabulary at the one place the decision
    // is taken (`MemberResolver.hpp`'s `ConstructorCall`, which is what turns
    // a call into `bConstructs`). Kept as its own constant here rather than
    // reached for across the Private/Passes boundary: this file runs at the
    // Driver seam, long before any `TypeCheckerContext` exists.
    constexpr std::string_view ConstructorCall = "new";

    // One analysable body: the member to stamp, and the unit AST its
    // declaration lives in. Built once, before the fixpoint, because
    // `Member` addresses are stable only while nothing is added to the store
    // — which holds here (`SynthesizeFinalizeStubs`, the last pass that adds
    // members, has already run).
    struct Body
    {
        Member *Owner                   = nullptr;
        const Frontend::AstContext *Ast = nullptr;
        // The nominal declaring it, so a `self.helper( … )` call inside the
        // body resolves precisely instead of falling back to the name index.
        NominalId Self;
    };

    // Every member in the store that spells `Name` and could actually *be*
    // the callee.
    //
    // The fallback when a call's receiver type is not syntactically knowable
    // — which is most calls, since this seam runs before any expression has
    // a type. Used all-or-nothing: a call is `Owned` only when *every*
    // member that could answer to that spelling returns owned, so the actual
    // callee (necessarily one of them) does too. That makes the
    // approximation **sound** — never a false `Owned` — at the cost of
    // precision: one borrowing `to_string` in the corpus demotes every
    // `to_string` call to `Borrowed`, i.e. to a counted leak.
    //
    // An `abstract def` is deliberately excluded, and that exclusion is what
    // makes the device usable at all rather than a precision detail. A
    // contract has no body, so nothing can ever prove it and it drags every
    // spelling it names down with it — `mixin Arithmetic`'s `abstract def +`
    // alone demoted *every* `+` in the corpus, `String#+` included, which is
    // the single most common owned-producing expression there is. It is also
    // never the callee: a contract is answered either by a concrete member
    // (which is in this index on its own account) or, on a primitive/pointer
    // layout, by the backend — and no machine scalar is a finalize candidate,
    // so the answer cannot matter there. An `@[External]` member stays in:
    // it *is* a real implementation, just one with no body to read, so it
    // must keep demoting its spelling.
    using NameIndex = std::unordered_map<std::string_view, std::vector<const Member *>>;

    [[nodiscard]] NameIndex BuildNameIndex ( const TypeStore &Store )
    {
        NameIndex Index;
        for ( std::size_t TypeIdx = 0; TypeIdx < Store.TypeCount(); ++TypeIdx )
        {
            const NominalId Id{ static_cast<NominalId::ValueType>( TypeIdx ) };
            for ( const Member &Entry : Store.Type( Id ).Members )
            {
                if ( Entry.Kind == EMemberKind::Method and not Entry.bAbstract )
                {
                    Index[Store.Text( Entry.Name )].push_back( &Entry );
                }
            }
        }
        for ( const Member &Entry : Store.FreeFunctions() )
        {
            if ( not Entry.bAbstract )
            {
                Index[Store.Text( Entry.Name )].push_back( &Entry );
            }
        }
        return Index;
    }

    // Does every member answering to `Name` return owned? False for an
    // unknown spelling — nothing resolved, nothing proven.
    [[nodiscard]] bool AllNamedReturnOwned ( const NameIndex &Index, const std::string_view Name )
    {
        const auto It = Index.find( Name );
        if ( It == Index.end() or It->second.empty() )
        {
            return false;
        }
        for ( const Member *Entry : It->second )
        {
            if ( not Entry->bReturnsOwned )
            {
                return false;
            }
        }
        return true;
    }

    // The nominal a receiver expression *names*, when it names one at all:
    // `String.owned( … )` and `Pointer<UInt8>.malloc( … )` are static calls
    // whose target is exact, and resolving them precisely is what keeps the
    // stdlib's own constructor helpers from being demoted by the name index.
    [[nodiscard]] NominalId StaticReceiverNominal ( const Frontend::AstContext &Ast, const TypeStore &Store, Frontend::ExprId Id )
    {
        if ( not Id.IsValid() )
        {
            return NominalId{};
        }
        if ( const auto *Inst = std::get_if<Frontend::GenericInst>( &Ast.Expr( Id ) ) )
        {
            return StaticReceiverNominal( Ast, Store, Inst->Base );
        }
        const auto *Ident = std::get_if<Frontend::Identifier>( &Ast.Expr( Id ) );
        if ( Ident == nullptr )
        {
            return NominalId{};
        }
        const std::optional<NominalId> Found = Store.LookupType( Ast.Text( Ident->Name ) );
        return Found.has_value() ? *Found : NominalId{};
    }

    // The state one body's analysis carries: which of its locals hold an
    // owned value. Rebuilt from scratch on every fixpoint round, because a
    // callee flipping to owned can promote a local that was borrowed before.
    struct LocalOwnership
    {
        std::unordered_set<std::uint32_t> Owned;
    };

    // Is `Id` an expression that *produces* a value its consumer owns?
    //
    // The table `rules`-side reasoning demands, and deliberately nothing
    // more: a construction, a call to something already proven, a local
    // holding one, or a branch whose every arm qualifies. Everything else —
    // a bare read, a field, a dereference, an unresolvable call — is
    // `Borrowed`, which is the safe direction.
    [[nodiscard]] bool ProducesOwned ( const Frontend::AstContext &Ast,
                                       const TypeStore &Store,
                                       const NameIndex &Index,
                                       const Body &Owner,
                                       const LocalOwnership &Locals,
                                       Frontend::ExprId Id );

    // `receiver.name` used as an invocation: does the member it names hand
    // back an owned value?
    //
    // Shared by the two spellings of one call. `x.dup()` reaches it as a
    // `Call`'s callee; `x.dup` — the paren-less form Volt writes far more
    // often, and the one every stdlib one-liner body ends on — reaches it as a
    // bare `Member` standing in value position. Answering them differently is
    // how `def mk( x ) -> String; x.dup; end` came to be classified as
    // borrowing while `def mk( x ) -> String; x.dup(); end` was not.
    [[nodiscard]] bool MemberInvocationReturnsOwned ( const Frontend::AstContext &Ast,
                                                      const TypeStore &Store,
                                                      const NameIndex &Index,
                                                      const Body &Owner,
                                                      const Frontend::Member &MemberNode )
    {
        const std::string_view Name = Ast.Text( MemberNode.Name );

        // A construction always yields fresh storage — the one case that needs
        // no proof at all, and the same fact `Resolution::bConstructs` records
        // at a call site.
        if ( Name == ConstructorCall )
        {
            return true;
        }

        // `String.owned( … )` / `self.helper( … )`: an exact receiver, so
        // resolve on it rather than through the name index.
        NominalId Receiver = StaticReceiverNominal( Ast, Store, MemberNode.Object );
        if ( not Receiver.IsValid() and MemberNode.Object.IsValid() and
             std::holds_alternative<Frontend::SelfExpr>( Ast.Expr( MemberNode.Object ) ) )
        {
            Receiver = Owner.Self;
        }
        if ( Receiver.IsValid() )
        {
            const TypeStore::MemberRef Found = Store.LookupMember( Receiver, Name );
            if ( Found.Decl != nullptr )
            {
                // A field is a *place*, never an invocation: reading it hands
                // back a view its owner still holds.
                return Found.Decl->Kind == EMemberKind::Method and Found.Decl->bReturnsOwned;
            }
        }
        return AllNamedReturnOwned( Index, Name );
    }

    // A body's value on some path out of it — its own last expression
    // statement. Anything else (a trailing `while`, an empty body) yields
    // nothing and is treated as not-owned.
    [[nodiscard]] bool BodyProducesOwned ( const Frontend::AstContext &Ast,
                                           const TypeStore &Store,
                                           const NameIndex &Index,
                                           const Body &Owner,
                                           const LocalOwnership &Locals,
                                           const Frontend::StmtList &Stmts )
    {
        if ( Stmts.IsEmpty() )
        {
            return false;
        }
        const auto *Tail = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Stmts[Stmts.Size() - 1] ) );
        return Tail != nullptr and ProducesOwned( Ast, Store, Index, Owner, Locals, Tail->Expr );
    }

    bool ProducesOwned ( const Frontend::AstContext &Ast,
                         const TypeStore &Store,
                         const NameIndex &Index,
                         const Body &Owner,
                         const LocalOwnership &Locals,
                         const Frontend::ExprId Id )
    {
        if ( not Id.IsValid() )
        {
            return false;
        }
        const Frontend::ExprNode &Node = Ast.Expr( Id );

        // A literal materialises a value; it cannot name a place, so there is
        // nothing for it to alias and nobody else to hold what it produces.
        // `""` becomes a `String` construction, `[ … ]` an `Array` built
        // element by element, a closure literal a `Proc` over a fresh
        // environment — all of them later, inside `TypeChecker`, which is why
        // reading the *lowering* would be guesswork; reading what a literal
        // **is** is not.
        //
        // Which node kinds are literals is not a list this file keeps either:
        // a type states its own claim with `@[Literal( Kind )]`, and
        // `LookupNodeKind` reads it back — the same mechanism that identifies
        // `nil`, the pointee type and the callable type
        // (rules/zero-hardcode.md). No Volt type name and no node kind is
        // spelled here, and a stdlib that claims a new one is covered with no
        // edit.
        if ( Store.LookupNodeKind( Frontend::NodeName( Node ) ).has_value() )
        {
            return true;
        }

        // A name holding an owned value hands that ownership on: `result =
        // Array<U>.new; …; result` is exactly how `Enumerable#map` is
        // written, and refusing it would leave every stdlib collection
        // combinator classified as borrowing.
        if ( const auto *Ident = std::get_if<Frontend::Identifier>( &Node ) )
        {
            return Locals.Owned.contains( Ident->Name.Value );
        }

        // A paren-less invocation, `x.dup` — the same node kind a *place* read
        // uses, which is why the resolved member's `Kind` is the discriminator
        // rather than the syntax (`Lifetime/ExprOwnership.hpp` draws the
        // identical line one phase later, from the resolution). A field read
        // falls through to `false`, which is the safe answer for it anyway.
        if ( const auto *MemberNode = std::get_if<Frontend::Member>( &Node ) )
        {
            return MemberInvocationReturnsOwned( Ast, Store, Index, Owner, *MemberNode );
        }

        if ( const auto *CallNode = std::get_if<Frontend::Call>( &Node ) )
        {
            if ( not CallNode->Callee.IsValid() )
            {
                return false;
            }
            const Frontend::ExprNode &Callee = Ast.Expr( CallNode->Callee );

            // The callee is a closure literal standing right there, so its
            // body *is* the value and can simply be read — the same question,
            // one level in. This is the shape every desugared composition and
            // pipeline produces (`x |> (&.trim) >> (&.downcase)` lowers to
            // nested `Call( Lambda, … )`), and the one case where a call
            // through a callable is not opaque at all.
            if ( const auto *LambdaNode = std::get_if<Frontend::Lambda>( &Callee ) )
            {
                return ProducesOwned( Ast, Store, Index, Owner, Locals, LambdaNode->Body );
            }
            if ( const auto *BlockNode = std::get_if<Frontend::Block>( &Callee ) )
            {
                return BodyProducesOwned( Ast, Store, Index, Owner, Locals, BlockNode->Body );
            }

            if ( const auto *MemberNode = std::get_if<Frontend::Member>( &Callee ) )
            {
                return MemberInvocationReturnsOwned( Ast, Store, Index, Owner, *MemberNode );
            }
            if ( const auto *Ident = std::get_if<Frontend::Identifier>( &Callee ) )
            {
                const Member *Found = Store.LookupFunction( Ast.Text( Ident->Name ) );
                return Found != nullptr and Found->bReturnsOwned;
            }
            return false;
        }

        // An operator is an ordinary member call on a non-primitive receiver
        // (rules/core-ast.md's operator contract), and `String#+` is the
        // single most common owned-producing expression in the corpus. On a
        // primitive receiver the spelling resolves to nothing that returns
        // owned anyway, so no separate layout test is needed here.
        if ( const auto *Bin = std::get_if<Frontend::Binary>( &Node ) )
        {
            return AllNamedReturnOwned( Index, Frontend::TokenSpelling( Bin->Op ) );
        }
        if ( const auto *Un = std::get_if<Frontend::Unary>( &Node ) )
        {
            return AllNamedReturnOwned( Index, Frontend::TokenSpelling( Un->Op ) );
        }

        // A branch produces an owned value only if *every* arm does — one
        // borrowing arm makes the whole expression unsafe to finalize.
        if ( const auto *Tern = std::get_if<Frontend::Ternary>( &Node ) )
        {
            return ProducesOwned( Ast, Store, Index, Owner, Locals, Tern->Then ) and
                   ProducesOwned( Ast, Store, Index, Owner, Locals, Tern->Else );
        }
        if ( const auto *IfNode = std::get_if<Frontend::If>( &Node ) )
        {
            return BodyProducesOwned( Ast, Store, Index, Owner, Locals, IfNode->Then ) and
                   BodyProducesOwned( Ast, Store, Index, Owner, Locals, IfNode->Else );
        }
        if ( const auto *CaseNode = std::get_if<Frontend::CaseExpr>( &Node ) )
        {
            for ( const Frontend::StmtId ClauseId : CaseNode->Clauses )
            {
                const auto &Clause = std::get<Frontend::WhenClause>( Ast.Stmt( ClauseId ) );
                if ( not BodyProducesOwned( Ast, Store, Index, Owner, Locals, Clause.Body ) )
                {
                    return false;
                }
            }
            return BodyProducesOwned( Ast, Store, Index, Owner, Locals, CaseNode->ElseBody );
        }
        if ( const auto *BeginNode = std::get_if<Frontend::BeginExpr>( &Node ) )
        {
            if ( not BodyProducesOwned( Ast, Store, Index, Owner, Locals, BeginNode->Body ) )
            {
                return false;
            }
            for ( const Frontend::StmtId RescueId : BeginNode->RescueClauses )
            {
                const auto &Rescue = std::get<Frontend::RescueClause>( Ast.Stmt( RescueId ) );
                if ( not BodyProducesOwned( Ast, Store, Index, Owner, Locals, Rescue.Body ) )
                {
                    return false;
                }
            }
            return true;
        }

        return false;
    }

    // --- Body traversal ---------------------------------------------------

    // A read-only reflective descent over every statement reachable from a
    // body, expression-position control constructs included
    // (`Meta::ForEachField`, never a switch over kinds —
    // rules/meta-first.md). Read-only, so the arena-rewrite hazard
    // (rules/ast-rewrite.md) does not apply: nothing here calls `Add()`.
    template <typename Fn> void VisitStmtDeep ( const Frontend::AstContext &Ast, Frontend::StmtId Id, Fn &&Visit );
    template <typename Fn> void VisitExprDeep ( const Frontend::AstContext &Ast, Frontend::ExprId Id, Fn &&Visit );

    template <typename NodeVariant, typename Fn>
    void VisitFieldsDeep ( const Frontend::AstContext &Ast, const NodeVariant &Variant, Fn &&Visit )
    {
        std::visit(
            [&] ( const auto &Node )
            {
                using T = std::remove_cvref_t<decltype( Node )>;
                if constexpr ( not std::is_same_v<T, std::monostate> )
                {
                    Meta::ForEachField( Node,
                                        [&] ( const char *, const auto &Field )
                                        {
                                            using F = std::remove_cvref_t<decltype( Field )>;
                                            if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                            {
                                                VisitExprDeep( Ast, Field, Visit );
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                            {
                                                for ( const Frontend::ExprId Child : Field )
                                                {
                                                    VisitExprDeep( Ast, Child, Visit );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                            {
                                                VisitStmtDeep( Ast, Field, Visit );
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                            {
                                                for ( const Frontend::StmtId Child : Field )
                                                {
                                                    VisitStmtDeep( Ast, Child, Visit );
                                                }
                                            }
                                        } );
                }
            },
            Variant );
    }

    template <typename Fn> void VisitStmtDeep ( const Frontend::AstContext &Ast, const Frontend::StmtId Id, Fn &&Visit )
    {
        if ( not Id.IsValid() )
        {
            return;
        }
        Visit( Id );
        VisitFieldsDeep( Ast, Ast.Stmt( Id ), Visit );
    }

    template <typename Fn> void VisitExprDeep ( const Frontend::AstContext &Ast, const Frontend::ExprId Id, Fn &&Visit )
    {
        if ( not Id.IsValid() )
        {
            return;
        }
        VisitFieldsDeep( Ast, Ast.Expr( Id ), Visit );
    }

    // Which of this body's locals hold an owned value, to fixpoint *within*
    // the body — `a = String.new( … ); b = a; b` needs two rounds, and a
    // body deep enough to need more than a handful is a body this analysis
    // is content to under-approximate (which is, again, a leak, not a
    // corruption).
    [[nodiscard]] LocalOwnership CollectOwnedLocals ( const Frontend::AstContext &Ast,
                                                      const TypeStore &Store,
                                                      const NameIndex &Index,
                                                      const Body &Owner,
                                                      const Frontend::StmtList &Stmts )
    {
        constexpr int MaxRounds = 4;

        LocalOwnership Locals;
        for ( int Round = 0; Round < MaxRounds; ++Round )
        {
            const std::size_t Before = Locals.Owned.size();
            for ( const Frontend::StmtId Root : Stmts )
            {
                VisitStmtDeep( Ast, Root,
                               [&] ( const Frontend::StmtId Id )
                               {
                                   const Frontend::StmtNode &Node = Ast.Stmt( Id );
                                   if ( const auto *Local = std::get_if<Frontend::LocalDecl>( &Node ) )
                                   {
                                       if ( ProducesOwned( Ast, Store, Index, Owner, Locals, Local->Init ) )
                                       {
                                           Locals.Owned.insert( Local->Name.Value );
                                       }
                                       return;
                                   }
                                   const auto *ExprStmtNode = std::get_if<Frontend::ExprStmt>( &Node );
                                   if ( ExprStmtNode == nullptr )
                                   {
                                       return;
                                   }
                                   const auto *AssignNode = std::get_if<Frontend::Assign>( &Ast.Expr( ExprStmtNode->Expr ) );
                                   if ( AssignNode == nullptr or not AssignNode->Target.IsValid() )
                                   {
                                       return;
                                   }
                                   const auto *Target = std::get_if<Frontend::Identifier>( &Ast.Expr( AssignNode->Target ) );
                                   if ( Target != nullptr and
                                        ProducesOwned( Ast, Store, Index, Owner, Locals, AssignNode->Value ) )
                                   {
                                       Locals.Owned.insert( Target->Name.Value );
                                   }
                               } );
            }
            if ( Locals.Owned.size() == Before )
            {
                break;
            }
        }
        return Locals;
    }

    // Does this body hand an owned value back on *every* path that returns
    // one? Collected over the explicit `return`s plus the body's own tail;
    // a single borrowing path disqualifies the whole member, and a body
    // with no value-yielding path at all is not owned either.
    [[nodiscard]] bool BodyReturnsOwned ( const TypeStore &Store, const NameIndex &Index, const Body &Owner )
    {
        const Frontend::AstContext &Ast = *Owner.Ast;
        const auto *MethodNode          = std::get_if<Frontend::Method>( &Ast.Decl( Owner.Owner->Decl ) );
        if ( MethodNode == nullptr or MethodNode->bAbstract or MethodNode->bExternal or MethodNode->Body.IsEmpty() )
        {
            return false;
        }

        const LocalOwnership Locals = CollectOwnedLocals( Ast, Store, Index, Owner, MethodNode->Body );

        bool bAnyPath  = false;
        bool bAllOwned = true;
        for ( const Frontend::StmtId Root : MethodNode->Body )
        {
            VisitStmtDeep( Ast, Root,
                           [&] ( const Frontend::StmtId Id )
                           {
                               const auto *ReturnNode = std::get_if<Frontend::Return>( &Ast.Stmt( Id ) );
                               if ( ReturnNode == nullptr or not ReturnNode->Value.IsValid() )
                               {
                                   return;
                               }
                               bAnyPath  = true;
                               bAllOwned = bAllOwned and ProducesOwned( Ast, Store, Index, Owner, Locals, ReturnNode->Value );
                           } );
        }

        // The implicit tail is a returning path too — and the only one, for
        // the overwhelming majority of Volt bodies.
        if ( const auto *Tail = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( MethodNode->Body[MethodNode->Body.Size() - 1] ) ) )
        {
            bAnyPath  = true;
            bAllOwned = bAllOwned and ProducesOwned( Ast, Store, Index, Owner, Locals, Tail->Expr );
        }

        return bAnyPath and bAllOwned;
    }

    // --- Parameter escape --------------------------------------------------

    // Every member answering to `Name`, or nothing at all. The same
    // all-or-nothing device `AllNamedReturnOwned` uses, in the opposite
    // direction: an argument is proven borrowed only when *every* member that
    // could answer to that spelling borrows it, so the actual callee
    // (necessarily one of them) does too.
    [[nodiscard]] bool AllNamedBorrowParam ( const NameIndex &Index, const std::string_view Name, const std::size_t Positional )
    {
        const auto It = Index.find( Name );
        if ( It == Index.end() or It->second.empty() )
        {
            return false;
        }
        for ( const Member *Entry : It->second )
        {
            if ( ParameterEscapes( *Entry, Positional ) )
            {
                return false;
            }
        }
        return true;
    }

    // The spelling a callee expression names, when it names one. A `Member`
    // callee (`arr.push( … )`) and a bare `Identifier` callee (an
    // implicit-`self` method or a top-level `def`) are the only two shapes
    // that reach a declared parameter list; anything else — an indirect call
    // through a value, a callee still buried in un-lowered sugar — names
    // nothing and leaves its arguments escaping.
    [[nodiscard]] std::string_view CalleeSpelling ( const Frontend::AstContext &Ast, const Frontend::ExprId Callee )
    {
        if ( not Callee.IsValid() )
        {
            return {};
        }
        const Frontend::ExprNode &Node = Ast.Expr( Callee );
        if ( const auto *MemberNode = std::get_if<Frontend::Member>( &Node ) )
        {
            return Ast.Text( MemberNode->Name );
        }
        if ( const auto *Ident = std::get_if<Frontend::Identifier>( &Node ) )
        {
            return Ast.Text( Ident->Name );
        }
        return {};
    }

    // One body's escape analysis: which of its own parameters it can be seen
    // to keep.
    //
    // The walk carries a single bit per child slot — "may a value read here
    // stay behind after the call" — and the default is **no**. Only the
    // positions listed in `WalkExpr` grant it, so a parameter reaching any
    // node this file does not model (un-lowered sugar included: this runs at
    // the Driver seam, before `EPassKind::Lowering`) is reported as escaping
    // rather than silently borrowed.
    struct EscapeScan
    {
        const Frontend::AstContext &Ast;
        const NameIndex &Index;
        // Parameter name -> its slot in `Member::Params`, by AST spelling —
        // the same by-name device the return-ownership fixpoint above uses,
        // since no ScopeTable exists at this seam.
        std::unordered_map<std::uint32_t, std::size_t> ByName;
        std::vector<bool> &Escapes;

        void Mark ( const Core::Symbol Name )
        {
            if ( const auto Found = ByName.find( Name.Value ); Found != ByName.end() )
            {
                Escapes[Found->second] = true;
            }
        }

        void WalkBody ( const Frontend::StmtList &Body )
        {
            for ( const Frontend::StmtId Id : Body )
            {
                WalkStmt( Id );
            }
        }

        // Every child of a node this walk has no rule for, at the default —
        // nothing here grants a borrow. Written once with `Meta::ForEachField`
        // so a node added to `Nodes.inl` is covered on the safe side with no
        // edit (rules/meta-first.md).
        template <typename NodeVariant> void WalkFields ( const NodeVariant &Variant )
        {
            std::visit(
                [&] ( const auto &Node )
                {
                    using T = std::remove_cvref_t<decltype( Node )>;
                    if constexpr ( not std::is_same_v<T, std::monostate> )
                    {
                        Meta::ForEachField( Node,
                                            [&] ( const char *, const auto &Field )
                                            {
                                                using F = std::remove_cvref_t<decltype( Field )>;
                                                if constexpr ( std::is_same_v<F, Frontend::ExprId> )
                                                {
                                                    WalkExpr( Field, false );
                                                }
                                                else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                                {
                                                    for ( const Frontend::ExprId Child : Field )
                                                    {
                                                        WalkExpr( Child, false );
                                                    }
                                                }
                                                else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                                {
                                                    WalkStmt( Field );
                                                }
                                                else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                                {
                                                    WalkBody( Field );
                                                }
                                            } );
                    }
                },
                Variant );
        }

        void WalkStmt ( const Frontend::StmtId Id )
        {
            if ( not Id.IsValid() )
            {
                return;
            }
            const Frontend::StmtNode &Node = Ast.Stmt( Id );

            // A name written to is a name this by-spelling analysis can no
            // longer follow: after `p = something_else`, an occurrence of `p`
            // is not the parameter. Refusing the whole slot is the cheap safe
            // answer, and reassigning a parameter is rare enough that the
            // precision is not missed.
            if ( const auto *Local = std::get_if<Frontend::LocalDecl>( &Node ) )
            {
                Mark( Local->Name );
            }

            WalkFields( Node );
        }

        // `bBorrowOk` is true when a value read at this slot cannot outlive
        // the enclosing call — a receiver, a dereferenced pointer, an
        // argument the callee was itself proven to borrow.
        void WalkExpr ( const Frontend::ExprId Id, const bool bBorrowOk )
        {
            if ( not Id.IsValid() )
            {
                return;
            }
            const Frontend::ExprNode &Node = Ast.Expr( Id );

            if ( const auto *Ident = std::get_if<Frontend::Identifier>( &Node ) )
            {
                if ( not bBorrowOk )
                {
                    Mark( Ident->Name );
                }
                return;
            }

            // A receiver is read, never kept — the one documented coarseness
            // here: a method that stores its own `self` somewhere would defeat
            // it. Nothing in the corpus does, and modelling `self` escape is
            // its own analysis.
            if ( const auto *MemberNode = std::get_if<Frontend::Member>( &Node ) )
            {
                WalkExpr( MemberNode->Object, true );
                return;
            }
            if ( const auto *DerefNode = std::get_if<Frontend::Deref>( &Node ) )
            {
                WalkExpr( DerefNode->Operand, true );
                return;
            }

            // `( x : Int32 )` states a type and hands the value straight on,
            // so it takes the position it stands in.
            if ( const auto *TypedNode = std::get_if<Frontend::TypedExpr>( &Node ) )
            {
                WalkExpr( TypedNode->Value, bBorrowOk );
                return;
            }
            if ( const auto *UnaryNode = std::get_if<Frontend::Unary>( &Node ) )
            {
                WalkExpr( UnaryNode->Operand, true );
                return;
            }
            if ( const auto *BinaryNode = std::get_if<Frontend::Binary>( &Node ) )
            {
                // An operator on a non-primitive receiver *is* a member call
                // (rules/core-ast.md's operator contract), so its right-hand
                // side is that member's first positional argument.
                WalkExpr( BinaryNode->Lhs, true );
                WalkExpr( BinaryNode->Rhs, AllNamedBorrowParam( Index, Frontend::TokenSpelling( BinaryNode->Op ), 0 ) );
                return;
            }

            // A branch hands its own value to whatever position the branch
            // itself occupies, so the arms inherit rather than reset.
            if ( const auto *TernaryNode = std::get_if<Frontend::Ternary>( &Node ) )
            {
                WalkExpr( TernaryNode->Cond, true );
                WalkExpr( TernaryNode->Then, bBorrowOk );
                WalkExpr( TernaryNode->Else, bBorrowOk );
                return;
            }

            if ( const auto *AssignNode = std::get_if<Frontend::Assign>( &Node ) )
            {
                // Writing *through* a place reads the place; the value
                // written is what stays behind.
                WalkExpr( AssignNode->Target, true );
                WalkExpr( AssignNode->Value, false );
                return;
            }

            if ( const auto *CallNode = std::get_if<Frontend::Call>( &Node ) )
            {
                WalkExpr( CallNode->Callee, true );

                const std::string_view Name = CalleeSpelling( Ast, CallNode->Callee );
                // A construction keeps everything it is given, by definition
                // — the same fact `Resolution::bConstructs` records at a call
                // site. A named argument is refused outright rather than
                // matched: positional index is the only mapping onto
                // `Member::Params` this seam can make without the resolution
                // the parallel TypeChecker has not built yet.
                const bool bConstructing = Name == ConstructorCall;
                bool bNamed              = false;
                for ( const Core::Symbol ArgName : CallNode->ArgNames )
                {
                    bNamed = bNamed or ArgName.IsValid();
                }

                for ( std::size_t Arg = 0; Arg < CallNode->Args.Size(); ++Arg )
                {
                    const bool bBorrows =
                        not bConstructing and not bNamed and not Name.empty() and AllNamedBorrowParam( Index, Name, Arg );
                    WalkExpr( CallNode->Args[Arg], bBorrows );
                }
                // A trailing block is a callable value handed to the callee's
                // own `&block` slot. Left escaping: the slot is not
                // positional, and no parameter in the corpus is ever passed
                // into one.
                WalkExpr( CallNode->BlockArg, false );
                return;
            }

            // Everything else — an `If`/`CaseExpr`/`BeginExpr` in expression
            // position, a `RaiseExpr`, and every sugar node still standing at
            // this seam — grants nothing.
            WalkFields( Node );
        }
    };

    // Every member in the store whose body this build can actually see.
    //
    // A cache-hit stdlib slot is deliberately absent: its flags arrived with
    // the cache, and re-deriving them would need an AST that was never
    // parsed. Shared by both fixpoints below — they analyse the same set of
    // bodies, only the question differs.
    [[nodiscard]] std::vector<Body> CollectBodies ( TypeStore &Store, const std::span<const Frontend::AstContext *const> Units )
    {
        std::vector<Body> Bodies;
        for ( std::size_t TypeIdx = 0; TypeIdx < Store.TypeCount(); ++TypeIdx )
        {
            const NominalId Id{ static_cast<NominalId::ValueType>( TypeIdx ) };
            for ( Member &Entry : Store.MutableMembers( Id ) )
            {
                if ( Entry.Kind != EMemberKind::Method or Entry.Unit >= Units.size() or Units[Entry.Unit] == nullptr )
                {
                    continue;
                }
                Bodies.push_back( Body{ .Owner = &Entry, .Ast = Units[Entry.Unit], .Self = Id } );
            }
        }
        for ( Member &Entry : Store.MutableFreeFunctions() )
        {
            if ( Entry.Unit >= Units.size() or Units[Entry.Unit] == nullptr )
            {
                continue;
            }
            Bodies.push_back( Body{ .Owner = &Entry, .Ast = Units[Entry.Unit], .Self = NominalId{} } );
        }
        return Bodies;
    }

    // Which of `Owner`'s parameters this round can see escaping. Recomputed
    // from scratch each round: a callee flipping to "borrows" can only ever
    // clear an escape, never add one.
    [[nodiscard]] std::vector<bool> ScanParamEscape ( const NameIndex &Index, const Body &Owner )
    {
        const Frontend::AstContext &Ast = *Owner.Ast;
        const std::size_t Count         = Owner.Owner->Params.Size();

        std::vector<bool> Escapes( Count, true );

        const auto *MethodNode = std::get_if<Frontend::Method>( &Ast.Decl( Owner.Owner->Decl ) );
        if ( MethodNode == nullptr or MethodNode->bAbstract or MethodNode->bExternal or MethodNode->Params.Size() != Count )
        {
            return Escapes;
        }

        // Nothing seen yet: assume borrowed, then let the walk contradict it.
        // An empty body contradicts nothing, which is correct — a parameter
        // no statement mentions cannot be kept.
        Escapes.assign( Count, false );

        EscapeScan Scan{ .Ast = Ast, .Index = Index, .ByName = {}, .Escapes = Escapes };
        for ( std::size_t Slot = 0; Slot < Count; ++Slot )
        {
            const Frontend::Param &ParamNode = Ast.GetParam( MethodNode->Params[Slot] );
            // `def initialize( @x : T )` stores its argument into the field
            // without a statement to show for it, so no walk can ever see the
            // escape. Marked here, at the one place the shorthand is visible.
            if ( ParamNode.bInstanceVar )
            {
                Escapes[Slot] = true;
                continue;
            }
            Scan.ByName[ParamNode.Name.Value] = Slot;
        }
        Scan.WalkBody( MethodNode->Body );
        return Escapes;
    }

} // namespace

bool ParameterEscapes ( const Member &Decl, const std::size_t Index )
{
    std::size_t Positional = 0;
    for ( std::size_t Slot = 0; Slot < Decl.Params.Size(); ++Slot )
    {
        if ( Slot < Decl.ParamIsBlock.Size() and Decl.ParamIsBlock[Slot] )
        {
            continue;
        }
        if ( Positional == Index )
        {
            return Slot >= Decl.ParamEscapes.Size() or Decl.ParamEscapes[Slot];
        }
        ++Positional;
    }
    // Past the end: a default-valued slot the call did not fill, or an arity
    // this analysis cannot match. Nothing to release either way.
    return true;
}

Core::SmallVec<bool, 4>
ClosureParameterEscape ( const Frontend::AstContext &Ast, const TypeStore &Store, const Frontend::ExprId Id )
{
    if ( not Id.IsValid() )
    {
        return {};
    }

    const Frontend::ParamList *Params = nullptr;
    const Frontend::ExprNode &Node    = Ast.Expr( Id );
    if ( const auto *LambdaNode = std::get_if<Frontend::Lambda>( &Node ) )
    {
        Params = &LambdaNode->Params;
    }
    else if ( const auto *BlockNode = std::get_if<Frontend::Block>( &Node ) )
    {
        Params = &BlockNode->Params;
    }
    if ( Params == nullptr )
    {
        return {};
    }

    // Same starting point as `ScanParamEscape`: assume borrowed, let the walk
    // contradict it. A body that mentions nothing contradicts nothing, which
    // is correct — a parameter no statement reads cannot be kept.
    std::vector<bool> Escapes( Params->Size(), false );

    const NameIndex Index = BuildNameIndex( Store );
    EscapeScan Scan{ .Ast = Ast, .Index = Index, .ByName = {}, .Escapes = Escapes };
    for ( std::size_t Slot = 0; Slot < Params->Size(); ++Slot )
    {
        const Frontend::Param &ParamNode = Ast.GetParam( ( *Params )[Slot] );
        if ( ParamNode.bInstanceVar )
        {
            Escapes[Slot] = true;
            continue;
        }
        Scan.ByName[ParamNode.Name.Value] = Slot;
    }

    if ( const auto *LambdaNode = std::get_if<Frontend::Lambda>( &Node ) )
    {
        Scan.WalkExpr( LambdaNode->Body, false );
    }
    else
    {
        Scan.WalkBody( std::get<Frontend::Block>( Node ).Body );
    }

    Core::SmallVec<bool, 4> Out;
    for ( const bool Escaped : Escapes )
    {
        Out.PushBack( Escaped );
    }
    return Out;
}

bool BlockParameterEscapes ( const Member &Decl )
{
    for ( std::size_t Slot = 0; Slot < Decl.Params.Size(); ++Slot )
    {
        if ( Slot < Decl.ParamIsBlock.Size() and Decl.ParamIsBlock[Slot] )
        {
            return Slot >= Decl.ParamEscapes.Size() or Decl.ParamEscapes[Slot];
        }
    }
    return true;
}

void InferParameterEscape ( const std::span<const Frontend::AstContext *const> Units, TypeStore &Store )
{
    const NameIndex Index = BuildNameIndex( Store );

    // Size every slot that has never been sized, to all-escaping. A cache-hit
    // member is already sized and keeps the answer that arrived with the
    // cache — its body was never parsed, so re-deriving is not an option.
    const auto Initialize = [] ( Member &Entry )
    {
        if ( Entry.ParamEscapes.Size() == Entry.Params.Size() )
        {
            return;
        }
        Entry.ParamEscapes.Clear();
        for ( std::size_t Slot = 0; Slot < Entry.Params.Size(); ++Slot )
        {
            Entry.ParamEscapes.PushBack( true );
        }
    };

    for ( std::size_t TypeIdx = 0; TypeIdx < Store.TypeCount(); ++TypeIdx )
    {
        const NominalId Id{ static_cast<NominalId::ValueType>( TypeIdx ) };
        for ( Member &Entry : Store.MutableMembers( Id ) )
        {
            Initialize( Entry );
        }
    }
    for ( Member &Entry : Store.MutableFreeFunctions() )
    {
        Initialize( Entry );
    }

    const std::vector<Body> Bodies = CollectBodies( Store, Units );

    // Monotone downward: a slot only ever goes true -> false, every predicate
    // read is monotone in those flags, so the count of `true`s strictly
    // decreases each round until it stops. The bound mirrors
    // `InferReturnOwnership`'s, for the same belt-and-braces reason.
    for ( std::size_t Round = 0; Round <= Bodies.size(); ++Round )
    {
        bool bChanged = false;
        for ( const Body &Entry : Bodies )
        {
            const std::vector<bool> Scanned = ScanParamEscape( Index, Entry );
            for ( std::size_t Slot = 0; Slot < Scanned.size() and Slot < Entry.Owner->ParamEscapes.Size(); ++Slot )
            {
                if ( Entry.Owner->ParamEscapes[Slot] and not Scanned[Slot] )
                {
                    Entry.Owner->ParamEscapes[Slot] = false;
                    bChanged                        = true;
                }
            }
        }
        if ( not bChanged )
        {
            break;
        }
    }
}

void InferReturnOwnership ( const std::span<const Frontend::AstContext *const> Units, TypeStore &Store )
{
    const NameIndex Index = BuildNameIndex( Store );

    const std::vector<Body> Bodies = CollectBodies( Store, Units );

    // Monotone: a flag only ever goes false -> true, and every predicate
    // above is monotone in the flags it reads, so the number of `true`s
    // strictly increases each round until it stops. Bounded by that count,
    // hence by `Bodies.size()`; the loop condition is the fixpoint itself,
    // the bound is belt-and-braces against a predicate that stops being
    // monotone one day.
    for ( std::size_t Round = 0; Round <= Bodies.size(); ++Round )
    {
        bool bChanged = false;
        for ( const Body &Entry : Bodies )
        {
            if ( Entry.Owner->bReturnsOwned )
            {
                continue;
            }
            if ( BodyReturnsOwned( Store, Index, Entry ) )
            {
                Entry.Owner->bReturnsOwned = true;
                bChanged                   = true;
            }
        }
        if ( not bChanged )
        {
            break;
        }
    }
}

} // namespace Volt::Sema::Raii
