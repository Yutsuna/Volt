#include "Volt/MiddleEnd/Analysis/Lifetime/FieldCascade.hpp"

#include "ExprInferencer.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/MiddleEnd/Analysis/Lifetime/FinalizeCallBuilder.hpp"
#include "Volt/MiddleEnd/Analysis/Raii/Ownership.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeResolve.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Volt::MiddleEnd::Analysis::Lifetime
{

namespace
{

    // A struct/class's own `finalize` should not need to hand-enumerate every
    // Aggregate-typed field itself (Exception#finalize did, by hand, before
    // this). Scoped to a type that already declares its own `finalize` —
    // synthesizing a brand-new Method Decl for a type with *no* declared
    // `finalize` would need registering with TypeStore/ScopeResolver *before*
    // TypeChecker runs (the same reason ClosureLifting's synthesized functions
    // get their own dedicated SynthesizedFunctions table rather than an
    // ordinary Decl append at this pass's own late stage — a Decl added here
    // would never be visible to TypeBinder's member table or the backend's own
    // per-type emission walk). rules/raii-ownership.md tracks that half separately.
    //
    // Generic types are *not* excluded (they were, until Phase 4): a field
    // written `Array<HashEntry<K,V>>` names a head nominal that declares
    // `finalize` whatever its arguments are, so it cascades under the ordinary
    // deferred-typing convention with no bound and no instantiation. Only a
    // field written as a *bare* parameter stays undecidable — see
    // `CollectCascadeFields`.

    // Forward declarations — a StmtId's own fields and an ExprId's own fields
    // recurse into each other, same shape as ContainsExitStmt/ContainsExitExpr
    // above.
    void CollectHandFinalizedFieldsStmt ( const Frontend::AstContext &Ast,
                                          Frontend::StmtId Id,
                                          std::unordered_set<std::uint32_t> &Out );
    void
    CollectHandFinalizedFields ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::unordered_set<std::uint32_t> &Out );

    // Same reflective-descent shape as ScanExitFields above (Meta::ForEachField,
    // not a hand-picked switch over every node kind — rules/meta-first.md), but
    // collecting a set instead of a bool: every `@field.finalize` (or
    // `@field.finalize()`) call reachable anywhere in a user-written `finalize`
    // body, keyed by the InstanceVar's own interned spelling (kept with its
    // leading `@`, matching `BuildFieldFinalizeCall`'s own `AtName`).
    template <typename NodeVariant>
    void ScanHandFinalizedFields ( const Frontend::AstContext &Ast,
                                   const NodeVariant &Variant,
                                   std::unordered_set<std::uint32_t> &Out )
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
                                                CollectHandFinalizedFields( Ast, Field, Out );
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::ExprList> )
                                            {
                                                for ( const Frontend::ExprId Child : Field )
                                                {
                                                    CollectHandFinalizedFields( Ast, Child, Out );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::StmtId> )
                                            {
                                                CollectHandFinalizedFieldsStmt( Ast, Field, Out );
                                            }
                                            else if constexpr ( std::is_same_v<F, Frontend::StmtList> )
                                            {
                                                for ( const Frontend::StmtId Child : Field )
                                                {
                                                    CollectHandFinalizedFieldsStmt( Ast, Child, Out );
                                                }
                                            }
                                        } );
                }
            },
            Variant );
    }

    void CollectHandFinalizedFieldsStmt ( const Frontend::AstContext &Ast,
                                          Frontend::StmtId Id,
                                          std::unordered_set<std::uint32_t> &Out )
    {
        if ( not Id.IsValid() )
        {
            return;
        }
        ScanHandFinalizedFields( Ast, Ast.Stmt( Id ), Out );
    }

    void
    CollectHandFinalizedFields ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::unordered_set<std::uint32_t> &Out )
    {
        if ( not Id.IsValid() )
        {
            return;
        }
        const Frontend::ExprNode &Node = Ast.Expr( Id );
        if ( const auto *CallNode = std::get_if<Frontend::Call>( &Node ) )
        {
            if ( CallNode->Callee.IsValid() )
            {
                if ( const auto *MemberNode = std::get_if<Frontend::Member>( &Ast.Expr( CallNode->Callee ) ) )
                {
                    if ( MemberNode->Object.IsValid() and
                         Ast.Text( MemberNode->Name ) == MiddleEnd::Analysis::Raii::FinalizeName )
                    {
                        if ( const auto *IVarNode = std::get_if<Frontend::InstanceVar>( &Ast.Expr( MemberNode->Object ) ) )
                        {
                            Out.insert( IVarNode->Name.Value );
                        }
                    }
                }
            }
        }
        ScanHandFinalizedFields( Ast, Node, Out );
    }

    // Every Field declared directly in Body whose resolved type is an
    // Aggregate-layout, finalize-declaring candidate — resolved straight off
    // each Field's own written TypeRef (ResolveTypeExpr), never through
    // TypeStore/NominalId machinery.
    //
    // `Generics` are the *enclosing* type's own parameter names, and passing
    // them is what makes this correct inside a generic body rather than merely
    // tolerated there. `Hash<K,V>`'s `@entries : Array<HashEntry<K,V>>`
    // resolves its head nominal (`Array`, which declares `finalize` whatever
    // its argument is — so the field is a genuine cascade candidate) while
    // `K`/`V` resolve through `UnitSink::Param` with no binding, i.e. to an
    // invalid id: the ordinary deferred-typing convention every generic body
    // already uses (core-ast.md §"Generic definition bodies"), and no bound is
    // required (rules/raii-ownership.md, applied one level up).
    //
    // A field whose written type is a *bare* parameter (`HashEntry<K,V>::key :
    // K`) therefore resolves to an invalid id and is skipped — it is not a
    // missed case handled by accident but the one documented remaining wall:
    // whether `K` needs finalizing is only knowable at instantiation.
    [[nodiscard]] std::vector<std::pair<Volt::Core::Symbol, MiddleEnd::TypeSystem::SemaTypeId>> CollectCascadeFields (
        TypeCheckerContext &Context, const Frontend::DeclList &Body, std::span<const Frontend::Symbol> Generics )
    {
        std::vector<std::pair<Volt::Core::Symbol, MiddleEnd::TypeSystem::SemaTypeId>> Result;
        const Frontend::AstContext &Ast = Context.Ctx.Ast;
        for ( const Frontend::DeclId Id : Body )
        {
            if ( not Id.IsValid() )
            {
                continue;
            }
            const auto *FieldNode = std::get_if<Frontend::Field>( &Ast.Decl( Id ) );
            if ( FieldNode == nullptr )
            {
                continue;
            }
            MiddleEnd::TypeSystem::UnitSink Sink{
                .Values = Context.Ctx.Values, .Self = MiddleEnd::TypeSystem::SemaTypeId{}, .Bindings = {} };
            const MiddleEnd::TypeSystem::SemaTypeId FieldType =
                MiddleEnd::TypeSystem::ResolveTypeExpr( Ast, Context.Ctx.Types, Generics, Sink, FieldNode->DeclType );
            if ( not Raii::IsFinalizeCandidateType( Context.Ctx.Types, Context.Ctx.Values, FieldType ) )
            {
                continue;
            }
            Result.emplace_back( FieldNode->Name, FieldType );
        }
        return Result;
    }

    // Every type this one destroys *through* — the written `< Parent` and each
    // `include`d mixin — whose own `finalize` does something.
    //
    // Resolved off the written type expression, exactly like a field's, and
    // filtered by exactly the same predicate: a parent is a cascade target for
    // the same reason a field is, and neither is a special case of the other.
    // A trivial parent is skipped for the same reason a trivial field is —
    // calling it would be a no-op, and `bTrivialFinalize` exists to say so.
    [[nodiscard]] std::vector<MiddleEnd::TypeSystem::SemaTypeId>
    CollectCascadeParents ( TypeCheckerContext &Context,
                            const Frontend::DeclList &Body,
                            std::span<const Frontend::Symbol> Generics,
                            Frontend::TypeId SuperWritten )
    {
        const Frontend::AstContext &Ast = Context.Ctx.Ast;

        const auto Resolve = [&] ( const Frontend::TypeId Written ) -> MiddleEnd::TypeSystem::SemaTypeId
        {
            if ( not Written.IsValid() )
            {
                return MiddleEnd::TypeSystem::SemaTypeId{};
            }
            MiddleEnd::TypeSystem::UnitSink Sink{
                .Values = Context.Ctx.Values, .Self = MiddleEnd::TypeSystem::SemaTypeId{}, .Bindings = {} };
            return MiddleEnd::TypeSystem::ResolveTypeExpr( Ast, Context.Ctx.Types, Generics, Sink, Written );
        };

        std::vector<MiddleEnd::TypeSystem::SemaTypeId> Result;
        const auto Consider = [&] ( const Frontend::TypeId Written )
        {
            const MiddleEnd::TypeSystem::SemaTypeId Resolved = Resolve( Written );
            if ( Raii::IsFinalizeCandidateType( Context.Ctx.Types, Context.Ctx.Values, Resolved ) )
            {
                Result.push_back( Resolved );
            }
        };

        Consider( SuperWritten );
        for ( const Frontend::DeclId Id : Body )
        {
            if ( not Id.IsValid() )
            {
                continue;
            }
            if ( const auto *Included = std::get_if<Frontend::Include>( &Ast.Decl( Id ) ) )
            {
                Consider( Included->Target );
            }
        }
        return Result;
    }

    // The Struct/Class's own `finalize`, if directly declared in Body — a plain
    // AST scan, not a TypeStore lookup: answering "is one of Body's own
    // elements a Method named finalize" needs no member-resolution machinery.
    [[nodiscard]] std::optional<Frontend::DeclId> FindOwnFinalizeMethod ( const Frontend::AstContext &Ast,
                                                                          const Frontend::DeclList &Body )
    {
        for ( const Frontend::DeclId Id : Body )
        {
            if ( not Id.IsValid() )
            {
                continue;
            }
            if ( const auto *MethodNode = std::get_if<Frontend::Method>( &Ast.Decl( Id ) ) )
            {
                if ( Ast.Text( MethodNode->Name ) == MiddleEnd::Analysis::Raii::FinalizeName )
                {
                    return Id;
                }
            }
        }
        return std::nullopt;
    }

    // `@field.finalize()` — mirrors BuildFinalizeCall's own shape (manual type
    // on the receiver, InferExpr for the Member/Call, and the same
    // BuildFinalizeCallOnReceiver element cascade when @field's own type is
    // itself Array-shaped over a finalize-candidate element — .agents/
    // rules/raii-ownership.md needed no bound at all: the cascade is driven
    // structurally, off the field's element type declaring `finalize`, exactly
    // like every other candidacy check in this file), but the receiver is an
    // InstanceVar rather than an Identifier: field access needs no scope
    // Binding at all (ExprPlaceEmitter reads it off Frame().Self directly at
    // codegen), and resolving it through the ordinary InstanceVar arm of
    // InferExpr would need Context.SelfValue set to this type — not ambient
    // here, since this step runs over every Struct/Class Decl in the file,
    // outside any per-method walk. Setting the type directly (InferExpr's own
    // cache check memoizes it, ExprInferencer.cpp) sidesteps that without
    // needing to fake a Self context.
    [[nodiscard]] Frontend::ExprId BuildFieldFinalizeCall ( TypeCheckerContext &Context,
                                                            Volt::Core::SourceRange Loc,
                                                            Volt::Core::Symbol FieldName,
                                                            MiddleEnd::TypeSystem::SemaTypeId FieldType )
    {
        Frontend::AstContext &Ast = Context.Ctx.Ast;

        // The written spelling keeps its `@` (ExprPlaceEmitter's own doc
        // comment on InstanceVar) — Field.Name never carries one, so this
        // synthesizes the same spelling a hand-written `@message` token would
        // have interned.
        const Volt::Core::Symbol AtName = Ast.Strings().Intern( "@" + std::string( Ast.Text( FieldName ) ) );

        auto MakeReadId = [&] () -> Frontend::ExprId
        {
            const Frontend::ExprId IVarId = Ast.Add( Frontend::ExprNode{ Frontend::InstanceVar{ .Loc = Loc, .Name = AtName } } );
            Context.Ctx.Values.SetExprType( IVarId, FieldType );
            return IVarId;
        };

        return BuildFinalizeCallOnReceiver( Context, Loc, MakeReadId, FieldType );
    }

    // `( self as Parent ).finalize()` — the delegation a destructor owes every
    // type it destroys *through*, and the exact counterpart of C++ running a
    // base destructor after the derived one's body.
    //
    // The receiver is a plain `SelfExpr` carrying the *parent's* type, which
    // is all it takes: a subclass's storage **is** one of its base
    // (`EnsureStructLayout` splices the base's fields in first, flat), so the
    // very same pointer is a valid receiver one level up, and `MemberType`
    // then resolves `finalize` on the parent exactly as a hand-written call
    // would. Typing the node here rather than letting `InferExpr` derive it is
    // both necessary — `Context.SelfValue` is not ambient in this per-Decl
    // sweep — and the whole mechanism: `self` typed as the parent is what
    // distinguishes delegation from unbounded self-recursion.
    [[nodiscard]] Frontend::ExprId BuildParentFinalizeCall ( TypeCheckerContext &Context,
                                                             Volt::Core::SourceRange Loc,
                                                             MiddleEnd::TypeSystem::SemaTypeId ParentType )
    {
        auto MakeReadId = [&] () -> Frontend::ExprId
        {
            const Frontend::ExprId SelfId = Context.Ctx.Ast.Add( Frontend::ExprNode{ Frontend::SelfExpr{ .Loc = Loc } } );
            Context.Ctx.Values.SetExprType( SelfId, ParentType );
            return SelfId;
        };

        return BuildFinalizeCallOnReceiver( Context, Loc, MakeReadId, ParentType );
    }

    // Appends the compiler-owed epilogue to a `finalize` method's own Body:
    // one `@field.finalize()` per cascade-candidate field, then one
    // `( self as Parent ).finalize()` per non-trivial parent.
    //
    // That order is C++'s, and for C++'s reason: a type's own members die
    // before the base subobject they were declared alongside. Fields go in
    // reverse declaration order (last-declared, first-finalized), matching
    // every other finalize ordering this pass already produces.
    //
    // Runs before InsertFinalizeCalls' own per-Method loop, so the ordinary
    // candidate/wrap machinery (ProcessBlock) sees these calls as part of the
    // body it instruments, exactly like a hand-written tail.
    void AppendFieldCascade ( TypeCheckerContext &Context,
                              Frontend::DeclId FinalizeMethodId,
                              const std::vector<std::pair<Volt::Core::Symbol, MiddleEnd::TypeSystem::SemaTypeId>> &Fields,
                              const std::vector<MiddleEnd::TypeSystem::SemaTypeId> &Parents )
    {
        Frontend::AstContext &Ast = Context.Ctx.Ast;

        // Copied out before any Add() below appends to the Expr/Stmt arenas
        // (rules/ast-rewrite.md).
        Frontend::Method Node = std::get<Frontend::Method>( Ast.Decl( FinalizeMethodId ) );

        // A user-declared `finalize` with a genuinely empty body (only the
        // cascade needed, nothing hand-written) is a legitimate shape — there is
        // no Body[0] to read a Loc off, so fall back to the Method's own.
        Volt::Core::SourceRange Loc = Node.Loc;
        if ( not Node.Body.IsEmpty() )
        {
            std::visit(
                [&] ( const auto &N )
                {
                    using T = std::remove_cvref_t<decltype( N )>;
                    if constexpr ( not std::is_same_v<T, std::monostate> )
                    {
                        Loc = N.Loc;
                    }
                },
                Ast.Stmt( Node.Body[Node.Body.Size() - 1] ) );
        }

        for ( auto It = Fields.rbegin(); It != Fields.rend(); ++It )
        {
            const Frontend::ExprId CallId = BuildFieldFinalizeCall( Context, Loc, It->first, It->second );
            Node.Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } ) );
        }
        for ( const MiddleEnd::TypeSystem::SemaTypeId ParentType : Parents )
        {
            const Frontend::ExprId CallId = BuildParentFinalizeCall( Context, Loc, ParentType );
            Node.Body.PushBack( Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Loc, .Expr = CallId } } ) );
        }

        Ast.Decl( FinalizeMethodId ) = Frontend::DeclNode{ std::move( Node ) };
    }

} // namespace

void RunFieldCascade ( TypeCheckerContext &Context )
{
    Frontend::AstContext &Ast = Context.Ctx.Ast;

    // Field cascade (rules/raii-ownership.md) — runs first, over
    // every Struct/Class Decl, so a type's own `finalize` (if it has one)
    // already carries its field-cascade epilogue by the time the ordinary
    // per-Method loop below instruments it. Never adds a Decl (only
    // appends Stmts to an *existing* Method's Body), so it needs no
    // DeclCount snapshot of its own — the arena's Decl slots are stable
    // across this loop.
    for ( std::size_t Index = 0; Index < Ast.DeclCount(); ++Index )
    {
        const Frontend::DeclId Id{ static_cast<Frontend::DeclId::ValueType>( Index ) };
        // A generic type is *not* excluded: `CollectCascadeFields` takes its
        // parameter names and defers on them, so `Hash<K,V>` cascades its
        // `Array`-typed field exactly like a concrete type would. Both spans
        // are copied out rather than kept as pointers into the Decl arena —
        // `AppendFieldCascade` below `Add()`s into it (rules/ast-rewrite.md).
        const Frontend::DeclList *BodyPtr = nullptr;
        std::span<const Frontend::Symbol> Generics;
        Frontend::TypeId SuperWritten;
        if ( const auto *StructNode = std::get_if<Frontend::Struct>( &Ast.Decl( Id ) ) )
        {
            BodyPtr  = &StructNode->Body;
            Generics = std::span<const Frontend::Symbol>{ StructNode->Generics.begin(), StructNode->Generics.Size() };
        }
        else if ( const auto *ClassNode = std::get_if<Frontend::Class>( &Ast.Decl( Id ) ) )
        {
            BodyPtr      = &ClassNode->Body;
            Generics     = std::span<const Frontend::Symbol>{ ClassNode->Generics.begin(), ClassNode->Generics.Size() };
            SuperWritten = ClassNode->Super;
        }
        if ( BodyPtr == nullptr )
        {
            continue;
        }
        const Frontend::DeclList BodyCopy = *BodyPtr;
        const std::vector<Frontend::Symbol> GenericsCopy{ Generics.begin(), Generics.end() };
        const Frontend::DeclList *Body = &BodyCopy;
        Generics                       = std::span<const Frontend::Symbol>{ GenericsCopy };

        const std::optional<Frontend::DeclId> FinalizeId = FindOwnFinalizeMethod( Ast, *Body );
        if ( not FinalizeId.has_value() )
        {
            continue;
        }
        std::vector<std::pair<Volt::Core::Symbol, MiddleEnd::TypeSystem::SemaTypeId>> Fields =
            CollectCascadeFields( Context, *Body, Generics );
        const std::vector<MiddleEnd::TypeSystem::SemaTypeId> Parents =
            CollectCascadeParents( Context, *Body, Generics, SuperWritten );

        // A user-written `finalize` may already call `@field.finalize`
        // itself (the only correct way to write one before this cascade
        // existed — Exception.vl's own former backtrace loop did exactly
        // this). Appending the cascade on top unconditionally would double
        // free that field (confirmed empirically: a `finalize` whose whole
        // body is `@s.finalize` had its single field freed twice once the
        // cascade landed). Only cascade fields the body does not already
        // finalize by hand.
        const auto &FinalizeMethod = std::get<Frontend::Method>( Ast.Decl( *FinalizeId ) );
        std::unordered_set<std::uint32_t> HandFinalized;
        for ( const Frontend::StmtId StmtId : FinalizeMethod.Body )
        {
            CollectHandFinalizedFieldsStmt( Ast, StmtId, HandFinalized );
        }
        std::erase_if( Fields,
                       [&] ( const std::pair<Volt::Core::Symbol, MiddleEnd::TypeSystem::SemaTypeId> &Entry )
                       {
                           const Volt::Core::Symbol AtName = Ast.Strings().Intern( "@" + std::string( Ast.Text( Entry.first ) ) );
                           return HandFinalized.contains( AtName.Value );
                       } );
        if ( Fields.empty() and Parents.empty() )
        {
            continue;
        }

        AppendFieldCascade( Context, *FinalizeId, Fields, Parents );
    }
}

} // namespace Volt::MiddleEnd::Analysis::Lifetime
