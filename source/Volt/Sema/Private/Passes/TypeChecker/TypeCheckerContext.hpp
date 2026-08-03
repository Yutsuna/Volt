#pragma once

#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"
#include "Volt/Sema/Pass.hpp"
#include "Volt/Sema/Scope/ScopeTable.hpp"

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Volt::Sema::TypeCheckerPass
{

// A member resolved on a receiver, with its already-instantiated
// result type and (for a method) its already-instantiated parameter
// types, in declaration order.
struct Resolution
{
    const Member *Decl = nullptr;
    SemaTypeId Result;
    Core::SmallVec<SemaTypeId, 4> Params;
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
    Core::SmallVec<SemaTypeId, 2> Bindings;
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

struct TypeCheckerContext
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

    // The `&block` slot the call currently being typed expects, handed
    // down to the trailing block's parameters. Set by CallType around
    // the BlockArg and consumed — once — by BindClosureParams, so a
    // closure nested deeper cannot inherit it by accident.
    SemaTypeId ExpectedClosure{};

    explicit TypeCheckerContext ( PassContext &InCtx, std::vector<bool> InMetadata );

    void Report ( Core::SourceRange Loc, std::string Message );

    // The declaration site behind a use: ScopeResolver's binding when the
    // node existed at Order 10, else the name index a previous write filled.
    [[nodiscard]] std::optional<BindingSite> SiteOf ( Frontend::ExprId Use, Symbol Name ) const;

    [[nodiscard]] std::optional<SemaTypeId> FindLocal ( Frontend::ExprId Use, Symbol Name ) const;

    void WriteLocal ( Frontend::ExprId Use, Symbol Name, SemaTypeId Type );

    void ConstrainExprType ( Frontend::ExprId Expr, SemaTypeId TargetType );

    // The concrete arguments the enclosing generic was fixed to, when this
    // walk is a re-instantiation (Sema::ReinstantiateBody). Empty in an
    // ordinary pass, where a written `T` inside a generic body is deferred by
    // design — see UnitSink::Bindings.
    Core::SmallVec<SemaTypeId, 2> Substitution{};

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

    [[nodiscard]] SemaTypeId MakeType ( NominalId Base, Core::SmallVec<SemaTypeId, 2> Args );
};

} // namespace Volt::Sema::TypeCheckerPass
