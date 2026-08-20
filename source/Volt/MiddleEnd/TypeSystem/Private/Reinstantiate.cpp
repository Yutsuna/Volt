// Reinstantiate.cpp — MiddleEnd::TypeSystem::ReinstantiateBody (Instantiate.hpp), the one
// semantic step a monomorphising backend drives from outside a per-unit pass
// run.
//
// This is the *same* machinery TypeChecker itself uses — TrailingType,
// InferExpr, ConstrainExprType — just invoked a second time, over a fresh
// UnitTypes/UnitCallees rather than the unit's own, and with the generic
// body's parameters bound from the already-resolved Member::Params (via the
// public Instantiate()) instead of re-derived from written syntax through
// ResolveTypeExpr/UnitSink, which structurally cannot carry a binding
// (UnitSink::Param always refuses — the parameter case exists for ordinary,
// concrete method bodies, where a written `T` can never appear). Once self
// and the parameters carry concrete SemaTypeIds, every expression built on
// them — calls, operators, field access — resolves through the exact same
// LookupMemberOn / Instantiate / UnifySig this file's callers already trust
// for a non-generic body, because it is the same code.

#include "Volt/MiddleEnd/TypeSystem/Instantiate.hpp"

#include "ClosureInferencer.hpp"
#include "LiteralInferencer.hpp"
#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/MiddleEnd/Analysis/InstantiationCache.hpp"
#include "Volt/MiddleEnd/Analysis/Lifetime/Temporaries.hpp"
#include "Volt/MiddleEnd/Analysis/TypeCheckerContext.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "Volt/MiddleEnd/IR/CalleeMap.hpp"
#include "Volt/MiddleEnd/IR/SynthesizedFunctions.hpp"
#include "Volt/MiddleEnd/Lowering/LoweringPasses.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeResolve.hpp"

#include <optional>
#include <utility>
#include <variant>

namespace
{

using namespace Volt;
using namespace Volt::MiddleEnd::TypeSystem;
using PassContext = Volt::MiddleEnd::Core::PassContext;
using PassStats   = Volt::MiddleEnd::Core::PassStats;
using namespace Volt::MiddleEnd::Resolver;
using CalleeEntry         = Volt::MiddleEnd::IR::CalleeEntry;
using SynthesizedFunction = Volt::MiddleEnd::IR::SynthesizedFunction;

// Consume one MonoRequest-encoded subtree from the front of `Cursor` and
// intern it into `Values`: the same pre-order (NominalId, ArgCount, args...)
// encoding TypeMapper::FlattenValueType writes and BackendCore::ArgSubtree
// reads, decoded here rather than shared with it — Sema must not depend on
// Backend, which owns that side of the currency.
[[nodiscard]] SemaTypeId InternNext ( UnitTypes &Values, std::span<const std::uint32_t> &Cursor )
{
    if ( Cursor.size() < 2 )
    {
        Cursor = {};
        return SemaTypeId{};
    }

    const NominalId Base{ Cursor[0] };
    const std::uint32_t Count = Cursor[1];
    Cursor                    = Cursor.subspan( 2 );

    ::Volt::Core::SmallVec<SemaTypeId, 2> Args;
    Args.Reserve( Count );
    for ( std::uint32_t Index = 0; Index < Count; ++Index )
    {
        Args.PushBack( InternNext( Values, Cursor ) );
    }
    return Values.Intern( SemaType{ .Base = Base, .Args = std::move( Args ) } );
}

// The generic parameter *names* a type wrote, as this unit's AST interned
// them. Needed because ResolveTypeExpr matches a written `T` by symbol: a
// re-instantiation can substitute a parameter index (UnitSink::Bindings) only
// once the name is recognised as one. Null for anything that declares none.
[[nodiscard]] const Frontend::SymbolList *GenericsOf ( const Frontend::AstContext &Ast, Frontend::DeclId Id )
{
    if ( not Id.IsValid() or Id.Value >= Ast.DeclCount() )
    {
        return nullptr;
    }
    return std::visit(
        Meta::Overloaded{
            [] ( const Frontend::Class &Node ) { return &Node.Generics; },
            [] ( const Frontend::Struct &Node ) { return &Node.Generics; },
            [] ( const Frontend::Mixin &Node ) { return &Node.Generics; },
            [] ( const Frontend::Enum &Node ) { return &Node.Generics; },
            [] ( const auto & ) -> const Frontend::SymbolList * { return nullptr; },
        },
        Ast.Decl( Id ) );
}

// The type that lexically *declares* Entry — never Owner (the receiver's own
// type): they differ whenever Entry is inherited from a mixin declared in a
// different unit (`Array<T> include Enumerable<T>`, `filter` written inside
// Enumerable.vl), and a `T` written in the method's own body must match
// names interned by *that* unit, not the receiver's — mixing the two is
// exactly the "wrong interner" GenericsOf's own comment warns about.
// ReceiverArgs stays correct regardless: Entry.Bindings (ExprResolvedCallEmitter.cpp's
// bInherited branch) is already positioned against the declaring type's own
// generic space, the same space Instantiate() reads Entry.Params/Result
// against via SigTypeId::ParamIndex — this only has to agree on the *names*.
[[nodiscard]] Frontend::DeclId DeclaringTypeOf ( const TypeStore &Store, const Member &Entry )
{
    for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
    {
        const NominalId Id{ static_cast<NominalId::ValueType>( Index ) };
        if ( Store.Type( Id ).Unit != Entry.Unit )
        {
            continue;
        }
        for ( const Member &Candidate : Store.Type( Id ).Members )
        {
            if ( Candidate.Unit == Entry.Unit and Candidate.Decl == Entry.Decl )
            {
                return Store.Type( Id ).Decl;
            }
        }
    }
    return Frontend::DeclId{};
}

// Clears the in-flight mark on every exit from the body below, including the
// early ones. Without it a request that bails out (a `Decl` that is not a
// Method) would leave its key marked forever, and every later request for the
// same instantiation would be reported as a cycle.
class InFlightScope
{

public:

    InFlightScope ( Volt::MiddleEnd::Analysis::InstantiationCache &InCache, Volt::MiddleEnd::Analysis::MonoKey InKey )
        : Cache( &InCache ), Key( std::move( InKey ) )
    {
    }

    InFlightScope ( const InFlightScope & )           = delete;
    InFlightScope ( InFlightScope && )                = delete;
    InFlightScope &operator=( const InFlightScope & ) = delete;
    InFlightScope &operator=( InFlightScope && )      = delete;

    ~InFlightScope ()
    {
        Cache->Leave( Key );
    }

private:

    Volt::MiddleEnd::Analysis::InstantiationCache *Cache = nullptr;
    Volt::MiddleEnd::Analysis::MonoKey Key;
};

} // namespace

Volt::MiddleEnd::TypeSystem::InstantiatedBody
Volt::MiddleEnd::TypeSystem::ReinstantiateBody ( const TypeStore &Store,
                                                 const Frontend::AstContext &Ast,
                                                 const ScopeTable &Scopes,
                                                 const Member &Entry,
                                                 NominalId Owner,
                                                 std::span<const std::uint32_t> FlatArgs )
{
    ( void )Scopes;
    InstantiatedBody Result;
    Result.Values.BindUniverse( Store.Universe() );

    // Decode the flattened bindings into the build's canonical dictionary:
    // NominalId is the cross-unit, instantiation-independent currency the
    // request arrives in, and interning it here produces exactly the same
    // SemaTypeIds any unit would have produced for the same written types —
    // which is what makes them usable as a cache key below.
    ::Volt::Core::SmallVec<SemaTypeId, 2> ReceiverArgs;
    std::span<const std::uint32_t> Cursor = FlatArgs;
    while ( not Cursor.empty() )
    {
        ReceiverArgs.PushBack( InternNext( Result.Values, Cursor ) );
    }

    // Memoised: re-typing a body is pure in its key, so the same instantiation
    // is computed once per build (InstantiationCache.hpp).
    Analysis::MonoKey Key{
        .Generation = Store.Generation(), .Unit = Entry.Unit, .Template = Entry.Decl, .Owner = Owner, .CanonicalArgs = {} };
    for ( const SemaTypeId Arg : ReceiverArgs )
    {
        Key.CanonicalArgs.PushBack( Arg );
    }

    Analysis::InstantiationCache &Cache = Analysis::InstantiationCache::Global();
    if ( std::optional<InstantiatedBody> Cached = Cache.Lookup( Key ); Cached.has_value() and Cached->bFullInstantiation )
    {
        return std::move( *Cached );
    }

    // A body that reached its own instantiation would recurse forever. Nothing
    // does today (the backend queues further requests rather than nesting
    // them), so this returns the empty overlay it has and counts the event
    // rather than trying to produce a half-typed one.
    if ( not Cache.Enter( Key ) )
    {
        return Result;
    }
    const InFlightScope InFlight( Cache, Key );

    const std::size_t OwnerGenericCount = Owner.IsValid() ? Store.Type( Owner ).Params.Size() : 0;
    ::Volt::Core::SmallVec<SemaTypeId, 2> OwnerArgs;
    for ( std::size_t Index = 0; Index < OwnerGenericCount and Index < ReceiverArgs.Size(); ++Index )
    {
        OwnerArgs.PushBack( ReceiverArgs[Index] );
    }
    const SemaTypeId Self =
        Owner.IsValid() ? Result.Values.Intern( SemaType{ .Base = Owner, .Args = std::move( OwnerArgs ) } ) : SemaTypeId{};

    // A scratch pass run: ordinary TypeChecker inference never writes through
    // Ast/Scopes, so borrowing the declaring unit's own is sound even though
    // PassContext's fields are mutable references — the same contract
    // DefineMember already leans on when it reads a UnitView's Ast/Scopes to
    // emit a concrete body. LowerClosureLit is the one exception below: it is
    // a Lowering-shaped rewrite invoked from here, and it may freely Add()
    // (every new node is instantiation-local, never revisited by anyone
    // else) but is told, via Context.Redirects, never to overwrite a slot
    // that already existed before this call — that slot's literal is shared
    // by every other instantiation of this same generic body.
    ::Volt::Core::DiagEngine::Bag ScratchDiags;
    PassStats ScratchStats;
    PassContext ScratchCtx{
        // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast) — PassContext's
        // Ast/Scopes are mutable references for the sake of a Lowering pass;
        // TypeChecker (Analysis) never writes through either, so borrowing
        // the declaring unit's own read-only instances is sound.
        .Ast    = const_cast<Frontend::AstContext &>( Ast ),
        .Types  = Store,
        .Values = Result.Values,
        .Scopes = const_cast<ScopeTable &>( Scopes ),
        // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
        .Diags   = ScratchDiags,
        .Stats   = ScratchStats,
        .Globals = nullptr,
        .Sources = nullptr,
        .Callees = &Result.Callees,
        .Synth   = Result.Synth,
    };

    if ( not Entry.Decl.IsValid() or Entry.Decl.Value >= Ast.DeclCount() )
    {
        return Result;
    }

    const auto *MethodNode = std::get_if<Frontend::Method>( &Ast.Decl( Entry.Decl ) );
    if ( MethodNode == nullptr )
    {
        return Result;
    }

    // A written `T` (or `U`) inside this body is answerable now: the
    // arguments are fixed, so `sizeof T` in `Pointer<T>#malloc` is a concrete
    // width rather than the deferral it is during the generic-shaped pass.
    // The names come from *Entry's own* declaring type, not Owner's
    // (DeclaringTypeOf's own comment) — an inherited default written in a
    // mixin declared in another unit still needs its `T` resolved against
    // that mixin's own generics, interned in this same Ast, for `Array<T>.new`
    // inside `Enumerable<T>#filter`'s own body to mean anything. The method's
    // own generics (`map<U>`) are concatenated after — the identical
    // "type generics first, then the method's" order `ReceiverArgs`/
    // `Context.Substitution` already carries (Member::OwnGenerics's comment),
    // so a positional name match against either still lands on the right slot.
    Frontend::SymbolList CombinedGenerics;
    if ( const Frontend::SymbolList *TypeGenerics = GenericsOf( Ast, DeclaringTypeOf( Store, Entry ) ); TypeGenerics != nullptr )
    {
        for ( const Frontend::Symbol Name : *TypeGenerics )
        {
            CombinedGenerics.PushBack( Name );
        }
    }
    for ( const Frontend::Symbol Name : MethodNode->Generics )
    {
        CombinedGenerics.PushBack( Name );
    }
    Analysis::TypeCheckerContext Context{ ScratchCtx, Analysis::MetadataExprs( Ast ) };
    Context.SelfType     = Owner;
    Context.SelfValue    = Self;
    Context.Redirects    = &Result.Redirects;
    Context.Substitution = ReceiverArgs;
    Context.SelfGenerics = &CombinedGenerics;
    // bGenericBody stays false (the default): every binding above is now
    // concrete, so nothing walked from here should defer — a node that still
    // cannot resolve is a genuine middle-end gap, not this instantiation's.

    std::size_t Index = 0;
    for ( const Frontend::ParamId ParamRef : MethodNode->Params )
    {
        if ( Index >= Entry.Params.Size() )
        {
            break;
        }
        const SemaTypeId ParamType = Instantiate( Store, Entry.Params[Index], ReceiverArgs, Self, Result.Values );
        const BindingSite Site{ ParamRef };
        Context.LocalTypes[Site]                          = ParamType;
        Context.LocalSites[Ast.GetParam( ParamRef ).Name] = Site;
        Context.Locals[Ast.GetParam( ParamRef ).Name]     = ParamType;
        Result.Values.SetSiteType( Site, ParamType );
        ++Index;
    }

    const SemaTypeId Trailing = Analysis::TrailingType( Context, MethodNode->Body );
    Result.ReturnType         = Trailing;
    if ( MethodNode->Body.Size() > 0 )
    {
        if ( const auto *Last = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( MethodNode->Body[MethodNode->Body.Size() - 1] ) );
             Last != nullptr and Trailing.IsValid() )
        {
            Context.ConstrainExprType( Last->Expr, Context.CurrentMethodReturnType );
        }
    }

    // Every Lambda/Block literal this walk actually reached now has a
    // concrete, non-deferred type in Result.Values (LowerClosureLit's guard
    // reads exactly that). The declaring unit's own generic-shaped pass left
    // each one un-lowered on purpose (the same guard, T unresolved there) —
    // there is nothing concrete to copy from it, so it is lowered here
    // instead, fresh, with Context.Redirects steering every rewrite into
    // Result.Redirects rather than the shared Ast slot: the exact literal
    // below is walked again, unchanged, by every other instantiation of this
    // generic method (ast-rewrite.md's sweep discipline — the arena is
    // bounded before the first Add() this loop causes).
    Lowering::AnalyzeClosureLiterals( Context );

    const std::size_t OriginalExprCount = Ast.ExprCount();
    for ( std::size_t ExprIndex = 0; ExprIndex < OriginalExprCount; ++ExprIndex )
    {
        const Frontend::ExprId CandidateId{ static_cast<Frontend::ExprId::ValueType>( ExprIndex ) };
        if ( not Result.Values.ExprType( CandidateId ).IsValid() )
        {
            continue;
        }
        if ( not std::holds_alternative<Frontend::Lambda>( Ast.Expr( CandidateId ) ) and
             not std::holds_alternative<Frontend::Block>( Ast.Expr( CandidateId ) ) )
        {
            continue;
        }
        Lowering::LowerClosureLit( Context, CandidateId );
    }

    // Full-expression temporaries, for this instantiation only.
    //
    // The declaring unit's own pass could not do this: inside a generic body
    // every type is deferred, so nothing there is classifiable as owned and
    // the sweep is a no-op by construction. Here every binding is concrete, so
    // it is the same sweep asking the same questions and getting answers —
    // and, exactly like `LowerClosureLit` above, it lands in
    // `Context.Redirects` rather than in the shared slot, because the next
    // instantiation of this body must still see the original.
    //
    // Its sibling `RunScopeCleanup` is deliberately not run: it rewrites
    // `StmtList`s, and a statement list is not something the redirect map can
    // express. A named local of a generic body is therefore still released
    // only where the shared pass could see it — a counted leak, and the one
    // half of this that is still open.
    // Every body swept here is copied out of the arena first, and that is not
    // a precaution: the loop above just `Add()`ed a `Method` Decl per lifted
    // closure, so `MethodNode` — read before it ran — already points into a
    // reallocated arena, and every sweep below `Add()`s again
    // (rules/ast-rewrite.md). Only the *lists* are copied; the sweeps rewrite
    // expression slots, never a `StmtList`, so the copies stay faithful.
    std::vector<Frontend::StmtList> Bodies;
    Bodies.push_back( std::get<Frontend::Method>( Ast.Decl( Entry.Decl ) ).Body );
    // Every closure this instantiation just lifted is a function too, and owns
    // its own temporaries — the composed body of `(&.trim) >> (&.downcase)`
    // holds the intermediate `trim` result and nothing else ever will. In an
    // ordinary unit `InsertFinalizeCalls` reaches these through the Decl arena
    // sweep; here they were created a moment ago, so they are swept directly.
    for ( const SynthesizedFunction &Synth : Result.Synth.All() )
    {
        if ( const auto *SynthNode = std::get_if<Frontend::Method>( &Ast.Decl( Synth.Decl ) ) )
        {
            Bodies.push_back( SynthNode->Body );
        }
    }
    for ( const Frontend::StmtList &Swept : Bodies )
    {
        static_cast<void>( Analysis::Lifetime::RunTemporaries( Context, Swept ) );
    }

    // Snapshot the resolutions this walk collected — the same final step
    // TypeChecker itself takes at the end of a unit's pass run.
    for ( const auto &[Value, Found] : Context.CalleeResolution )
    {
        Result.Callees.Set( Frontend::ExprId{ static_cast<std::uint32_t>( Value ) },
                            CalleeEntry{ .Decl              = Found.Decl,
                                         .Result            = Found.Result,
                                         .Params            = Found.Params,
                                         .BlockParam        = Found.BlockParam,
                                         .Bindings          = Found.Bindings,
                                         .Receiver          = Found.Receiver,
                                         .bConstructs       = Found.bConstructs,
                                         .bIndirect         = Found.bIndirect,
                                         .VTableSlot        = Found.VTableSlot,
                                         .bDynamicDispatch  = Found.bDynamicDispatch,
                                         .MachineConversion = Found.MachineConversion } );
    }

    Result.bFullInstantiation = true;
    Cache.Insert( std::move( Key ), Result );
    return Result;
}

void Volt::MiddleEnd::TypeSystem::FlattenValueType ( const UnitTypes &Values, SemaTypeId Id, std::vector<std::uint32_t> &Out )
{
    if ( not Values.Has( Id ) )
    {
        return;
    }

    const SemaType &Value = Values.Get( Id );
    Out.push_back( Value.Base.Value );
    Out.push_back( static_cast<std::uint32_t>( Value.Args.Size() ) );
    for ( const SemaTypeId Arg : Value.Args )
    {
        FlattenValueType( Values, Arg, Out );
    }
}

namespace
{

[[nodiscard]] SemaTypeId InternFrom ( const UnitTypes &Source, SemaTypeId Id, UnitTypes &Dest )
{
    if ( not Source.Has( Id ) )
    {
        return SemaTypeId{};
    }
    const SemaType &Type = Source.Get( Id );
    ::Volt::Core::SmallVec<SemaTypeId, 2> Args;
    for ( const SemaTypeId Arg : Type.Args )
    {
        Args.PushBack( InternFrom( Source, Arg, Dest ) );
    }
    return Dest.Intern( SemaType{ .Base = Type.Base, .Args = std::move( Args ) } );
}

} // namespace

Volt::MiddleEnd::TypeSystem::SemaTypeId
Volt::MiddleEnd::TypeSystem::InferMethodReturnType ( const TypeStore &Store,
                                                     const Frontend::AstContext &Ast,
                                                     const ScopeTable &Scopes,
                                                     const Member &Entry,
                                                     NominalId Owner,
                                                     std::span<const std::uint32_t> FlatArgs,
                                                     UnitTypes &DestValues )
{
    if ( not Entry.Decl.IsValid() or Entry.Decl.Value >= Ast.DeclCount() )
    {
        return SemaTypeId{};
    }

    const auto *MethodNode = std::get_if<Frontend::Method>( &Ast.Decl( Entry.Decl ) );
    if ( MethodNode == nullptr )
    {
        return SemaTypeId{};
    }

    DestValues.BindUniverse( Store.Universe() );

    UnitTypes LocalValues;
    LocalValues.BindUniverse( Store.Universe() );

    ::Volt::Core::SmallVec<SemaTypeId, 2> ReceiverArgs;
    std::span<const std::uint32_t> Cursor = FlatArgs;
    while ( not Cursor.empty() )
    {
        ReceiverArgs.PushBack( InternNext( LocalValues, Cursor ) );
    }

    Analysis::MonoKey Key{
        .Generation = Store.Generation(), .Unit = Entry.Unit, .Template = Entry.Decl, .Owner = Owner, .CanonicalArgs = {} };
    for ( const SemaTypeId Arg : ReceiverArgs )
    {
        Key.CanonicalArgs.PushBack( Arg );
    }

    Analysis::InstantiationCache &Cache = Analysis::InstantiationCache::Global();
    if ( std::optional<InstantiatedBody> Cached = Cache.Lookup( Key ); Cached.has_value() )
    {
        if ( Cached->ReturnType.IsValid() )
        {
            return InternFrom( Cached->Values, Cached->ReturnType, DestValues );
        }
    }

    if ( not Cache.Enter( Key ) )
    {
        return SemaTypeId{};
    }
    const InFlightScope InFlight( Cache, Key );

    Frontend::SymbolList CombinedGenerics;
    if ( const Frontend::SymbolList *TypeGenerics = GenericsOf( Ast, DeclaringTypeOf( Store, Entry ) ); TypeGenerics != nullptr )
    {
        for ( const Frontend::Symbol Name : *TypeGenerics )
        {
            CombinedGenerics.PushBack( Name );
        }
    }
    for ( const Frontend::Symbol Name : MethodNode->Generics )
    {
        CombinedGenerics.PushBack( Name );
    }

    ::Volt::Core::DiagEngine::Bag ScratchDiags;
    PassStats ScratchStats;
    IR::UnitCallees ScratchCallees;
    IR::SynthesizedFunctions ScratchSynth;

    PassContext ScratchCtx{
        .Ast     = const_cast<Frontend::AstContext &>( Ast ),
        .Types   = Store,
        .Values  = LocalValues,
        .Scopes  = const_cast<ScopeTable &>( Scopes ),
        .Diags   = ScratchDiags,
        .Stats   = ScratchStats,
        .Globals = nullptr,
        .Sources = nullptr,
        .Callees = &ScratchCallees,
        .Synth   = ScratchSynth,
    };

    Analysis::TypeCheckerContext Context{ ScratchCtx, Analysis::MetadataExprs( Ast ) };
    const std::size_t OwnerGenericCount = Owner.IsValid() ? Store.Type( Owner ).Params.Size() : 0;
    ::Volt::Core::SmallVec<SemaTypeId, 2> OwnerArgs;
    for ( std::size_t Index = 0; Index < OwnerGenericCount and Index < ReceiverArgs.Size(); ++Index )
    {
        OwnerArgs.PushBack( ReceiverArgs[Index] );
    }
    const SemaTypeId Self =
        Owner.IsValid() ? LocalValues.Intern( SemaType{ .Base = Owner, .Args = std::move( OwnerArgs ) } ) : SemaTypeId{};

    Context.SelfType     = Owner;
    Context.SelfValue    = Self;
    Context.Substitution = ReceiverArgs;
    Context.SelfGenerics = &CombinedGenerics;

    std::size_t Index = 0;
    for ( const Frontend::ParamId ParamRef : MethodNode->Params )
    {
        if ( Index >= Entry.Params.Size() )
        {
            break;
        }
        const SemaTypeId ParamType = Instantiate( Store, Entry.Params[Index], ReceiverArgs, Self, LocalValues );
        const BindingSite Site{ ParamRef };
        Context.LocalTypes[Site]                          = ParamType;
        Context.LocalSites[Ast.GetParam( ParamRef ).Name] = Site;
        Context.Locals[Ast.GetParam( ParamRef ).Name]     = ParamType;
        LocalValues.SetSiteType( Site, ParamType );
        ++Index;
    }
    Context.CurrentMethodReturnType = Instantiate( Store, Entry.Result, ReceiverArgs, Self, LocalValues );

    const SemaTypeId Trailing = Analysis::TrailingType( Context, MethodNode->Body );

    InstantiatedBody MinimalResult;
    MinimalResult.ReturnType = Trailing;
    const SemaTypeId Ret     = InternFrom( LocalValues, Trailing, DestValues );
    MinimalResult.Values     = std::move( LocalValues );
    Cache.Insert( std::move( Key ), std::move( MinimalResult ) );

    return Ret;
}
