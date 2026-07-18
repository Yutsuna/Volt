#pragma once

#include "Volt/Frontend/AST/Node.hpp"

namespace Volt
{

namespace Frontend
{

    // <tag attr="x" on:click={h}> children </tag>. Attribute names and
    // values are parallel arrays; a value is a StringLiteral (static) or an
    // arbitrary expression (dynamic `{expr}`). Children are JsxElement,
    // JsxText, JsxFragment or interpolation expressions.
    struct JsxElement
    {

        Core::SourceRange Loc;
        Symbol Tag;
        SymbolList AttrNames;
        ExprList AttrValues;
        ExprList Children;
        bool bSelfClosing = false;
    };

    // <> ... </> — a group of children with no wrapping element.
    struct JsxFragment
    {

        Core::SourceRange Loc;
        ExprList Children;
    };

    // Literal text between JSX tags.
    struct JsxText
    {

        Core::SourceRange Loc;
        Symbol Text;
    };

} // namespace Frontend

} // namespace Volt
