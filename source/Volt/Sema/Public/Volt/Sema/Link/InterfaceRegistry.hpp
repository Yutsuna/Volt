#pragma once

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Volt
{

namespace Sema
{

    // The cross-unit seam. Everything else in the pipeline is strictly
    // per-file; this registry is the one place where units see each other.
    // The Driver fills it in a *serial* phase between the parallel parse and
    // the parallel sema, so passes may read it without locks but never write
    // to it. Names are plain strings because every unit keeps its own
    // private StringInterner — Symbols are not comparable across units.

    /// One exported top-level declaration of one unit, as seen from others.
    struct ExportedDecl
    {

        std::string QualifiedName; // e.g. "Core::AppConfig"
        Frontend::DeclKind Kind = Frontend::DeclKind::None;
        std::uint32_t Unit      = 0; // ordinal of the owning CompileUnit
        Frontend::DeclId Decl;       // into that unit's AstContext
    };

    class InterfaceRegistry
    {

    public:

        /// Serial publication phase only — not thread-safe by design.
        /// Last publication of a name wins, matching stdlib resolution order.
        void Publish ( ExportedDecl Exported )
        {
            const ExportedDecl &Stored = Decls.emplace_back( std::move( Exported ) );
            ByName.insert_or_assign( std::string_view{ Stored.QualifiedName }, &Stored );
        }

        /// Read-only lookup, safe from any sema worker thread.
        [[nodiscard]] const ExportedDecl *Lookup ( std::string_view QualifiedName ) const
        {
            if ( const auto It = ByName.find( QualifiedName ); It != ByName.end() )
            {
                return It->second;
            }
            return nullptr;
        }

        [[nodiscard]] std::size_t Size () const
        {
            return Decls.size();
        }

    private:

        // deque: stable addresses, so ByName may key on views into Decls.
        std::deque<ExportedDecl> Decls;
        std::unordered_map<std::string_view, const ExportedDecl *> ByName;
    };

    /// Walk Ast's top-level declarations (recursing through `module` nesting)
    /// and publish every named declaration under its qualified name. Returns
    /// the number of declarations published.
    std::size_t PublishUnitInterface ( const Frontend::AstContext &Ast, std::uint32_t Unit, InterfaceRegistry &Registry );

} // namespace Sema

} // namespace Volt
