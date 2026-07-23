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
