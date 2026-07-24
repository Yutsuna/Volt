#pragma once

#include "Sema_export.hpp"
#include "Volt/Core/Diagnostics/SourceLocation.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/AST/Expr.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Volt
{

namespace Sema
{

    // Everything an expansion is allowed to read, resolved once per use site.
    // Widening what magic constants can say is a field here plus a manifest
    // line — never a new branch. Function is empty outside a method body.
    struct MagicSite
    {

        std::string_view Path;     // SourceManager::PathOf, verbatim
        std::string_view Dir;      // the parent directory of Path
        std::string_view Function; // enclosing method name, "" at top level
        std::uint32_t Line   = 1;  // 1-based, from SourceManager::Resolve
        std::uint32_t Column = 1;  // 1-based
    };

    /// The literal node Name expands to at Site, or nullopt if Name is not a
    /// magic constant. Generated wholesale from MagicConstants.inl.
    [[nodiscard]] SEMA_EXPORT std::optional<Frontend::ExprNode>
    ExpandMagic ( std::string_view Name, const MagicSite &Site, Core::StringInterner &Interner, Core::SourceRange Loc );

    /// Whether Name has the reserved shape `__NAME__` — leading and trailing
    /// `__`, interior upper-case letters, digits or `_`. A name of this shape
    /// that ExpandMagic rejects is a typo, and is reported as one.
    [[nodiscard]] SEMA_EXPORT bool IsMagicShape ( std::string_view Name );

    /// Every spelling the manifest declares, for the diagnostic note. Derived
    /// from the same manifest, so it cannot drift from what ExpandMagic knows.
    [[nodiscard]] SEMA_EXPORT std::span<const std::string_view> MagicNames ();

} // namespace Sema

} // namespace Volt
