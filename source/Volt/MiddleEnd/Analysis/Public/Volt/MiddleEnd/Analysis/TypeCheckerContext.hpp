#pragma once

#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/MiddleEnd/Analysis/AnalysisTypes.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "Volt/MiddleEnd/Resolver/ScopeTable.hpp"
#include "Volt/MiddleEnd/TypeSystem/MemoryLayout.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeResolve.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "VoltMiddleEndAnalysis_export.hpp"

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Volt::MiddleEnd::Analysis
{

struct Resolution
{
    const Member *Decl = nullptr;
    // The nominal whose body declares `Decl`, which on an inherited member is
    // not the receiver: `describe` reached from a `Square` is owned by
    // `Shape`. Kept because that is the type a visibility check is *against* —
    // `private` means "the declaring type", and a Member has no back-pointer
    // to the type holding it. Invalid for a free function, which has no owner
    // and no visibility to check.
    NominalId Owner;
    SemaTypeId Result;
    ::Volt::Core::SmallVec<SemaTypeId, 4> Params;
    // The instantiated `&block` slot, kept out of Params because it binds
    // through the call's trailing `do ... end` rather than positionally.
    // This is what gives a block's parameters their types: `each` on an
    // `Array<Int32>` declares `&block : T -> Void`, so the `| i |` of
    // `arr.each do | i |` is an Int32 without anyone writing it down.
    SemaTypeId BlockParam;

    // The parameter space the signature above was instantiated against:
    // the owner's arguments, then one slot per method generic. Those last
    // slots start invalid — nothing but the call can fill them — and the
    // types above are recomputed as inference closes them.
    ::Volt::Core::SmallVec<SemaTypeId, 2> Bindings;
    // Kept so that recomputation resolves `self` the same way the first
    // instantiation did.
    SemaTypeId Receiver;
    // `T.new( … )`: the callee is the initializer, and the call constructs.
    // See CalleeEntry::bConstructs — this is where that fact is decided.
    bool bConstructs = false;
    // `f( x )` on a callable. See CalleeEntry::bIndirect — decided here, in
    // the one place that knows the receiver claims the FuncType node kind.
    bool bIndirect = false;
};

struct VOLT_MIDDLEEND_ANALYSIS_EXPORT TypeCheckerContext
{
    PassContext &Ctx;
    std::vector<bool> Metadata;
    std::unordered_set<std::uint32_t> NakedTypeExprs{};
    bool bStaticContext = false;

    // The type whose body we are inside, and the AST symbols of its
    // generic parameters (matched by ResolveTypeExpr, which reads
    // written names out of *this* unit's interner).
    NominalId SelfType{};
    const Frontend::SymbolList *SelfGenerics = nullptr;
    SemaTypeId SelfValue{};

    // Are we inside a generic definition's own body — `Array<T>`, `map<U>`?
    // Every type written there that mentions a parameter resolves to nothing
    // (UnitSink::Param), by design, so the expressions built on it carry no
    // type either. That is deferral, not a gap: see UnitTypes::MarkDeferred.
    bool bGenericBody = false;

    // Locals of the method body being walked, keyed by the
    // declaration site ScopeResolver published: structural keys, so
    // two locals in sibling scopes can never collide.
    std::unordered_map<BindingSite, SemaTypeId, BindingSiteHash> LocalTypes{};
    // Name → site index for the identifiers the ScopeTable cannot know:
    // every node materialised *after* Order 10 (MacroExpansion,
    // AssignLowering's `x = x op v`, CaseLowering) carries no binding, so a
    // write through one of them has no site of its own and must reach the
    // site the name was declared at. Without it the two answers drift —
    // `result *= self` re-typed the name while the declaration site kept the
    // literal's type, and the trailing read got whichever it consulted.
    std::unordered_map<Symbol, BindingSite> LocalSites{};
    // Name-keyed types, for a local that has no site at all: a name first
    // written through a materialised node, which no scope ever declared.
    std::unordered_map<Symbol, SemaTypeId> Locals{};
    std::unordered_set<std::uint32_t> UnconstrainedLiterals{};
    std::unordered_map<Symbol, Frontend::ExprId> UnconstrainedVarInitializers{};

    // Is `Init` an initialiser that has *no type of its own* — an
    // unconstrained literal, or something that never resolved?
    //
    // Only such a local may be re-typed by the context that first uses it
    // (`h = 5381` becoming a UInt64 because `hash` returns one). A local whose
    // initialiser already had a determinate type keeps it: registering
    // `buf = Pointer<UInt8>.malloc( n )` as re-typable let the first
    // `memcpy( buf, ... )` rewrite it to `Pointer<Void>` for good, and every
    // later use then compared against the wrong type.
    [[nodiscard]] bool IsUnconstrainedInit ( Frontend::ExprId Init, SemaTypeId InitType ) const
    {
        return not InitType.IsValid() or UnconstrainedLiterals.contains( Init.Value );
    }
    // Names of locals declared with a type annotation but no initializer
    // (`x : T` without `= expr`). Marked on declaration, cleared on first
    // assignment, checked on every read — definite assignment analysis.
    std::unordered_set<Symbol> UninitializedLocals{};
    SemaTypeId CurrentMethodReturnType{};
    // The name of the method body being walked, which is the whole meaning of
    // `super`: it calls *this* method one level up, not some fixed spelling.
    // Invalid outside a method body, where `super` has nothing to name.
    Symbol CurrentMethodName{};

    // The innermost `rescue`s a bare `raise` is currently nested in, pushed
    // while typing each clause's body and popped after. An anonymous clause
    // (no bound name) still pushes an invalid Symbol, so a nested anonymous
    // rescue does not inherit an outer clause's bound variable.
    std::vector<Symbol> RescueVarStack{};

    // The resolution behind a `receiver.name` expression that turned
    // out to be a method, keyed by that Member expression's own Id —
    // filled when it is inferred, read back by the wrapping Call so
    // arguments can be checked against Member::Params without a
    // second lookup.
    std::unordered_map<std::uint32_t, Resolution> CalleeResolution{};

    // Expression sites the middle-end *built* rather than found, and which
    // therefore hand the surrounding region a value it now owns.
    //
    // A construction normally announces itself through
    // `Resolution::bConstructs` on the `Call` that performs it — which is why
    // `LowerStringLit` needs no entry here: it rewrites its slot into exactly
    // that `Call`. `LowerArrayLit`/`LowerHashLit` cannot, because building an
    // aggregate literal takes a *sequence* (construct, then one append per
    // element), so the slot becomes a `BeginExpr` whose value is a name — and
    // a name reads as a place, never as a construction. The fact is not
    // recoverable from the shape afterwards, so it is recorded by the only
    // code that has it.
    std::unordered_set<std::uint32_t> OwnedExprSites{};

    // Closure literals whose body was read and proven to hand back an owned
    // value, keyed by the literal's own expression id.
    //
    // The one thing `Member::bReturnsOwned` structurally cannot cover: a call
    // *through a callable* resolves to the `FuncType` claimant's `abstract
    // call`, which has no body, so ownership of its result is unprovable in
    // general (rules/raii-ownership.md). It stops being unprovable
    // exactly when the callee is a literal standing in the call's own callee
    // slot — the shape every desugared composition and pipeline produces
    // (`x |> (&.trim) >> (&.downcase)` lowers to nested
    // `Call( Lambda, … )`) — because then the body is right there to read.
    //
    // Filled by `LowerClosureLits` before it rewrites anything, so the id
    // survives the rewrite (the literal's slot becomes the `Proc.new` the
    // parent still calls through), and read by
    // `Lifetime/ExprOwnership.hpp`'s `ProducesOwnedValue`.
    std::unordered_set<std::uint32_t> OwnedClosureLiterals{};

    // Per closure literal, which of its own parameters its body keeps —
    // `Raii::ClosureParameterEscape`'s answer, cached at the literal's own
    // expression id and read back by `Lifetime/Temporaries.cpp` when it has to
    // decide whether an argument of an *indirect* call is the caller's to
    // release. Filled by `LowerClosureLits` alongside `OwnedClosureLiterals`,
    // and for the same reason: the callable's one declared member speaks for
    // every closure in the program, so it can hold neither answer.
    std::unordered_map<std::uint32_t, Volt::Core::SmallVec<bool, 4>> ClosureParamEscapes{};

    // The `&block` slot the call currently being typed expects, handed
    // down to the trailing block's parameters. Set by CallType around
    // the BlockArg and consumed — once — by BindClosureParams, so a
    // closure nested deeper cannot inherit it by accident.
    SemaTypeId ExpectedClosure{};

    // Non-null only for MiddleEnd::TypeSystem::ReinstantiateBody's walk. A generic body's
    // Lambda/Block literal is shared by every instantiation (ast-value.md:
    // one AST per file, not per instantiation), so ClosureLifting's rewrite
    // may not overwrite its slot the way it does for a concrete body's own
    // one-shot pass — a second instantiation would find its own literal
    // already replaced by the first's concretely-typed lowering. When set,
    // LowerClosureLit records `original -> replacement` here instead of
    // mutating Ast.Expr( original ) in place; BodyEmitter consults the same
    // map (MiddleEnd::IR::ExprRedirectMap, FunctionFrame::Redirects) once per
    // instantiation, so the one shared literal gets a fresh, correctly-typed
    // construction for each concrete instantiation without ever touching the
    // shared Ast.
    std::unordered_map<std::uint32_t, Frontend::ExprId> *Redirects = nullptr;

    explicit TypeCheckerContext ( PassContext &InCtx, std::vector<bool> InMetadata );

    void Report ( Volt::Core::SourceRange Loc, std::string Message );

    // The declaration site behind a use: ScopeResolver's binding when the
    // node existed at Order 10, else the name index a previous write filled.
    [[nodiscard]] std::optional<BindingSite> SiteOf ( Frontend::ExprId Use, Symbol Name ) const;

    [[nodiscard]] std::optional<SemaTypeId> FindLocal ( Frontend::ExprId Use, Symbol Name ) const;

    void WriteLocal ( Frontend::ExprId Use, Symbol Name, SemaTypeId Type );

    void ConstrainExprType ( Frontend::ExprId Expr, SemaTypeId TargetType );

    // The UnitSink every annotation in a *body* is resolved through — the four
    // fields each site used to repeat, plus the `typeof` hook, which is the
    // reason this exists at all: `UnitSink::InferHook` needs a walk to call
    // back into, and this is the walk. Building one by hand elsewhere is not
    // wrong, only weaker — a `typeof( x )` resolved through it answers nothing.
    [[nodiscard]] TypeSystem::UnitSink MakeSink ();

    // The concrete arguments the enclosing generic was fixed to, when this
    // walk is a re-instantiation (MiddleEnd::TypeSystem::ReinstantiateBody). Empty in an
    // ordinary pass, where a written `T` inside a generic body is deferred by
    // design — see UnitSink::Bindings.
    Volt::Core::SmallVec<SemaTypeId, 2> Substitution{};

    [[nodiscard]] std::span<const SemaTypeId> GenericBindings () const
    {
        return std::span<const SemaTypeId>{ Substitution.begin(), Substitution.Size() };
    }

    [[nodiscard]] std::span<const Symbol> Generics () const;

    [[nodiscard]] std::string NameOf ( NominalId Id ) const;

    [[nodiscard]] std::string NameOfValue ( SemaTypeId Id ) const;

    [[nodiscard]] static constexpr SemaTypeId NoReturnType ()
    {
        return SemaTypeId{ 0xFFFFFFFEu };
    }

    [[nodiscard]] bool IsNoReturn ( SemaTypeId Id ) const
    {
        return Id == NoReturnType();
    }

    [[nodiscard]] SemaTypeId UnifyBranchTypes ( SemaTypeId A, SemaTypeId B ) const
    {
        if ( IsNoReturn( A ) )
            return B;
        if ( IsNoReturn( B ) )
            return A;
        if ( not A.IsValid() )
            return B;
        if ( not B.IsValid() )
            return A;
        if ( A == B )
            return A;
        return SemaTypeId{};
    }

    [[nodiscard]] SemaTypeId MakeType ( NominalId Base, Volt::Core::SmallVec<SemaTypeId, 2> Args );
};

} // namespace Volt::MiddleEnd::Analysis
