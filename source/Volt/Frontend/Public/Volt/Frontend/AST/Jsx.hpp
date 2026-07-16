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

            using Self = JsxElement;

            Core::SourceRange Loc;
            Symbol            Tag;
            SymbolList        AttrNames;
            ExprList          AttrValues;
            ExprList          Children;
            bool              bSelfClosing = false;

            VOLT_FIELDS( Tag, AttrNames, AttrValues, Children, bSelfClosing )
        };

        // <> ... </> — a group of children with no wrapping element.
        struct JsxFragment
        {

            using Self = JsxFragment;

            Core::SourceRange Loc;
            ExprList          Children;

            VOLT_FIELDS( Children )
        };

        // Literal text between JSX tags.
        struct JsxText
        {

            using Self = JsxText;

            Core::SourceRange Loc;
            Symbol            Text;

            VOLT_FIELDS( Text )
        };

    }

}
