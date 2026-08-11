#include "Volt/Sema/Raii/OwnershipInference.hpp"

#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
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

    // Every member in the store that spells `Name`, whatever declares it.
    //
    // The fallback when a call's receiver type is not syntactically knowable
    // — which is most calls, since this seam runs before any expression has
    // a type. Used all-or-nothing: a call is `Owned` only when *every*
    // member that could answer to that spelling returns owned, so the actual
    // callee (necessarily one of them) does too. That makes the
    // approximation **sound** — never a false `Owned` — at the cost of
    // precision: one borrowing `to_string` in the corpus demotes every
    // `to_string` call to `Borrowed`, i.e. to a counted leak.
    using NameIndex = std::unordered_map<std::string_view, std::vector<const Member *>>;

    [[nodiscard]] NameIndex BuildNameIndex ( TypeStore &Store )
    {
        NameIndex Index;
        for ( std::size_t TypeIdx = 0; TypeIdx < Store.TypeCount(); ++TypeIdx )
        {
            const NominalId Id{ static_cast<NominalId::ValueType>( TypeIdx ) };
            for ( const Member &Entry : Store.MutableMembers( Id ) )
            {
                if ( Entry.Kind == EMemberKind::Method )
                {
                    Index[Store.Text( Entry.Name )].push_back( &Entry );
                }
            }
        }
        for ( const Member &Entry : Store.MutableFreeFunctions() )
        {
            Index[Store.Text( Entry.Name )].push_back( &Entry );
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

        // A name holding an owned value hands that ownership on: `result =
        // Array<U>.new; …; result` is exactly how `Enumerable#map` is
        // written, and refusing it would leave every stdlib collection
        // combinator classified as borrowing.
        if ( const auto *Ident = std::get_if<Frontend::Identifier>( &Node ) )
        {
            return Locals.Owned.contains( Ident->Name.Value );
        }

        if ( const auto *CallNode = std::get_if<Frontend::Call>( &Node ) )
        {
            if ( not CallNode->Callee.IsValid() )
            {
                return false;
            }
            const Frontend::ExprNode &Callee = Ast.Expr( CallNode->Callee );
            if ( const auto *MemberNode = std::get_if<Frontend::Member>( &Callee ) )
            {
                const std::string_view Name = Ast.Text( MemberNode->Name );

                // A construction always yields fresh storage — the one case
                // that needs no proof at all, and the same fact
                // `Resolution::bConstructs` records at a call site.
                if ( Name == ConstructorCall )
                {
                    return true;
                }

                // `String.owned( … )` / `self.helper( … )`: an exact
                // receiver, so resolve on it rather than through the name
                // index.
                NominalId Receiver = StaticReceiverNominal( Ast, Store, MemberNode->Object );
                if ( not Receiver.IsValid() and MemberNode->Object.IsValid() and
                     std::holds_alternative<Frontend::SelfExpr>( Ast.Expr( MemberNode->Object ) ) )
                {
                    Receiver = Owner.Self;
                }
                if ( Receiver.IsValid() )
                {
                    const TypeStore::MemberRef Found = Store.LookupMember( Receiver, Name );
                    if ( Found.Decl != nullptr )
                    {
                        return Found.Decl->bReturnsOwned;
                    }
                }
                return AllNamedReturnOwned( Index, Name );
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

} // namespace

void InferReturnOwnership ( const std::span<const Frontend::AstContext *const> Units, TypeStore &Store )
{
    const NameIndex Index = BuildNameIndex( Store );

    // Every member whose body this build can actually see. A cache-hit
    // stdlib slot is deliberately absent: its flag arrived with the cache,
    // and re-deriving it would need an AST that was never parsed.
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
