#pragma once

// ScopeTable.hpp — lexical scopes and name bindings, as values, per compile
// unit.
//
// ScopeResolver (Order 10) builds this once and publishes it on the
// PassContext; later passes never re-resolve a name, they query the flat
// tables. Mirrors UnitTypes (SemaType.hpp): an arena of value scopes plus
// dense indexes keyed by AST Id, never a pointer graph. Zero-hardcode by
// construction: a Binding ties an interned Symbol to an AST declaration
// site — no Volt type name ever enters this file.

#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/AST/Node.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Volt
{

namespace Sema
{

    using Core::Symbol;

    struct ScopeTag
    {
    };

    using ScopeId = Core::TypedId<ScopeTag>;

    // Diagnostic / future capture-analysis flavour only: resolution never
    // branches on it — scoping is structural, not semantic.
    enum class EScopeKind : std::uint8_t
    {
        Unit,   // root scope of a file (Module top-level)
        Type,   // body of a Class/Struct/Mixin/Component/Circuit
        Method, // params + body of a Method
        Block,  // params + body of a Block (`do |x| ... end`)
        Branch, // Then/Else of an If, Body of a While — a bare lexical block
    };

    // Where a name was declared: always a typed AST Id, never a pointer and
    // never a copy of the node.
    using BindingSite = std::variant<Frontend::StmtId,  // LocalDecl
                                     Frontend::ParamId, // Method/Block parameter
                                     Frontend::DeclId>; // Field / member decl

    // Structural key hash for maps keyed by BindingSite (TypeChecker's local
    // types): alternative index + the TypedId value inside it.
    struct BindingSiteHash
    {

        [[nodiscard]] std::size_t operator()( const BindingSite &Site ) const noexcept
        {
            const auto Value = std::visit( [] ( const auto &Id ) { return Id.Value; }, Site );
            return std::hash<std::size_t>{}( ( Site.index() << 32U ) ^ Value );
        }
    };

    // One named declaration, visible from Owner downwards.
    struct Binding
    {

        Symbol Name;
        BindingSite Site;
        ScopeId Owner;
    };

    // A variable or parameter from an outer scope captured by a closure.
    struct Capture
    {

        BindingSite Site;
        Symbol Name;
        ScopeId DeclaringScope;
        ScopeId ClosureScope;
    };

    // One lexical scope: shadowing is a per-scope entry, never a global one.
    struct Scope
    {

        ScopeId Parent; // invalid = root
        EScopeKind Kind = EScopeKind::Unit;
        Core::SmallVec<Symbol, 4> Order; // declaration order (stable diagnostics)
        std::unordered_map<Symbol, Binding> Bindings;
    };

    // The publication channel. Built by ScopeResolver, then read-only for
    // every later pass: BindingOf/ScopeOf are O(1) dense lookups, no chain
    // walking outside the resolver itself. Per-file state on the CompileUnit,
    // so the parallel sema phase needs no lock.
    class ScopeTable
    {

    public:

        [[nodiscard]] ScopeId PushScope ( ScopeId Parent, EScopeKind Kind )
        {
            return Scopes.Add( Scope{ .Parent = Parent, .Kind = Kind, .Order = {}, .Bindings = {} } );
        }

        // Declares Name at Site in InScope. Returns false when the name is
        // already declared *directly* in that scope (the caller diagnoses);
        // shadowing a parent scope is legal and silent.
        bool Declare ( ScopeId InScope, Symbol Name, BindingSite Site )
        {
            Scope &Target              = Scopes.Get( InScope );
            const auto [It, bInserted] = Target.Bindings.emplace( Name, Binding{ .Name = Name, .Site = Site, .Owner = InScope } );
            if ( bInserted )
            {
                Target.Order.PushBack( Name );
            }
            return bInserted;
        }

        // Full resolution, walking the parent chain — used once, by
        // ScopeResolver itself, at the point of use. The map nodes are heap
        // stable, so the returned pointer survives later PushScope calls.
        [[nodiscard]] const Binding *Resolve ( ScopeId From, Symbol Name ) const
        {
            for ( ScopeId It = From; It.IsValid(); It = Scopes.Get( It ).Parent )
            {
                const Scope &Current = Scopes.Get( It );
                if ( const auto Found = Current.Bindings.find( Name ); Found != Current.Bindings.end() )
                {
                    return &Found->second;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const Scope &Get ( ScopeId Id ) const
        {
            return Scopes.Get( Id );
        }

        [[nodiscard]] std::size_t Size () const
        {
            return Scopes.Size();
        }

        // --- Published table, consumed by later passes (O(1), no chain) ---

        void BindUse ( Frontend::ExprId Use, const Binding &Target )
        {
            if ( not Use.IsValid() )
            {
                return;
            }
            if ( Use.Value >= UseIndex.size() )
            {
                UseIndex.resize( static_cast<std::size_t>( Use.Value ) + 1, nullptr );
            }
            UseIndex[Use.Value] = &Target;
        }

        [[nodiscard]] const Binding *BindingOf ( Frontend::ExprId Use ) const
        {
            if ( not Use.IsValid() or Use.Value >= UseIndex.size() )
            {
                return nullptr;
            }
            return UseIndex[Use.Value];
        }

        void SetScopeOf ( Frontend::StmtId Stmt, ScopeId InScope )
        {
            if ( not Stmt.IsValid() )
            {
                return;
            }
            if ( Stmt.Value >= StmtScope.size() )
            {
                StmtScope.resize( static_cast<std::size_t>( Stmt.Value ) + 1, ScopeId{} );
            }
            StmtScope[Stmt.Value] = InScope;
        }

        [[nodiscard]] ScopeId ScopeOf ( Frontend::StmtId Stmt ) const
        {
            if ( not Stmt.IsValid() or Stmt.Value >= StmtScope.size() )
            {
                return ScopeId{};
            }
            return StmtScope[Stmt.Value];
        }

        void SetScopeOfExpr ( Frontend::ExprId Expr, ScopeId InScope )
        {
            if ( not Expr.IsValid() )
            {
                return;
            }
            if ( Expr.Value >= ExprScope.size() )
            {
                ExprScope.resize( static_cast<std::size_t>( Expr.Value ) + 1, ScopeId{} );
            }
            ExprScope[Expr.Value] = InScope;
        }

        [[nodiscard]] ScopeId ScopeOfExpr ( Frontend::ExprId Expr ) const
        {
            if ( not Expr.IsValid() or Expr.Value >= ExprScope.size() )
            {
                return ScopeId{};
            }
            return ExprScope[Expr.Value];
        }

        void RecordCapture ( ScopeId ClosureScope, const Binding &Target )
        {
            auto &Vec = ClosureCaptures[ClosureScope];
            for ( const auto &Existing : Vec )
            {
                if ( Existing.Site == Target.Site )
                {
                    return;
                }
            }
            Vec.PushBack( Capture{
                .Site           = Target.Site,
                .Name           = Target.Name,
                .DeclaringScope = Target.Owner,
                .ClosureScope   = ClosureScope,
            } );
        }

        [[nodiscard]] const Core::SmallVec<Capture, 4> *CapturesOf ( ScopeId Scope ) const
        {
            const auto It = ClosureCaptures.find( Scope );
            if ( It != ClosureCaptures.end() )
            {
                return &It->second;
            }
            return nullptr;
        }

        [[nodiscard]] const Core::SmallVec<Capture, 4> *CapturesOfExpr ( Frontend::ExprId Expr ) const
        {
            const ScopeId Id = ScopeOfExpr( Expr );
            return Id.IsValid() ? CapturesOf( Id ) : nullptr;
        }

        // A closure literal consumed directly where it is written — a call's
        // trailing `do ... end` / BlockArg, or a bare argument in its Args
        // list — never outlives that call: its environment can live on the
        // caller's stack. Anything else (bound to a local, returned, stored
        // in a field or a literal) may outlive the statement that wrote it,
        // so escaping is the conservative default absent a recorded proof.
        void SetEscapes ( ScopeId ClosureScope, bool bEscapes )
        {
            ClosureEscapes[ClosureScope] = bEscapes;
        }

        [[nodiscard]] bool Escapes ( ScopeId ClosureScope ) const
        {
            const auto It = ClosureEscapes.find( ClosureScope );
            return It == ClosureEscapes.end() or It->second;
        }

    private:

        Core::Arena<Scope, ScopeId> Scopes;
        std::vector<const Binding *> UseIndex; // by ExprId::Value
        std::vector<ScopeId> StmtScope;        // by StmtId::Value
        std::vector<ScopeId> ExprScope;        // by ExprId::Value
        std::unordered_map<ScopeId, Core::SmallVec<Capture, 4>> ClosureCaptures;
        std::unordered_map<ScopeId, bool> ClosureEscapes;
    };

} // namespace Sema

} // namespace Volt
