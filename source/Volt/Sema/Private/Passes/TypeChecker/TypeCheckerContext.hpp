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

    // Locals of the method body being walked, keyed by the
    // declaration site ScopeResolver published: structural keys, so
    // two locals in sibling scopes can never collide.
    std::unordered_map<BindingSite, SemaTypeId, BindingSiteHash> LocalTypes{};
    // Legacy name-keyed fallback, kept for the identifiers the
    // ScopeTable cannot know: nodes materialised after Order 10
    // (MacroExpansion, CaseLowering) and implicit `x = 5` assigns.
    std::unordered_map<Symbol, SemaTypeId> Locals{};
    std::unordered_set<std::uint32_t> UnconstrainedLiterals{};
    std::unordered_map<Symbol, Frontend::ExprId> UnconstrainedVarInitializers{};
    SemaTypeId CurrentMethodReturnType{};

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

    [[nodiscard]] std::optional<SemaTypeId> FindLocal ( Frontend::ExprId Use, Symbol Name ) const;

    void WriteLocal ( Frontend::ExprId Use, Symbol Name, SemaTypeId Type );

    void ConstrainExprType ( Frontend::ExprId Expr, SemaTypeId TargetType );

    [[nodiscard]] std::span<const Symbol> Generics () const;

    [[nodiscard]] std::string NameOf ( NominalId Id ) const;

    [[nodiscard]] std::string NameOfValue ( SemaTypeId Id ) const;

    [[nodiscard]] SemaTypeId MakeType ( NominalId Base, Core::SmallVec<SemaTypeId, 2> Args );
};

} // namespace Volt::Sema::TypeCheckerPass
