#pragma once

#include "Volt/Frontend/AST/AstContext.hpp"

namespace Volt
{

    namespace Frontend
    {

        /// AST-to-AST pass that rewrites every JSX node into ordinary calls onto
        /// the `Volt::JSX` runtime facade, keeping the tree free of JSX-specific
        /// nodes for later sema/codegen:
        ///
        ///   <div class="x">child</div>  ->  Volt::JSX.create_element("div",
        ///                                       { "class" => "x" }, [ child ])
        ///   <>a b</>                     ->  Volt::JSX.fragment([ a, b ])
        ///   plain text                   ->  Volt::JSX.text("...")
        ///
        /// Interpolation children (`{expr}`) are passed through unchanged inside
        /// the children array. Nodes are lowered in place: every reference is an
        /// ExprId, so replacing a slot rewrites the whole tree at once and order
        /// does not matter.
        class JsxLowering
        {

        public:

            explicit JsxLowering( AstContext& InContext ) : Context( InContext )
            {
            }

            /// Lower every JSX node currently in the context. Returns the number
            /// of nodes rewritten.
            std::size_t Run();

        private:

            /// `Volt::JSX.<Method>` — the member chain used as a call callee.
            [[nodiscard]] ExprId MakeFacade( std::string_view Method, Core::SourceRange Loc );

            // Each returns the rewritten node value; the caller assigns it back
            // into the original slot so parents keep referring to the same Id.
            // Callers pass a copy that outlives the arena reallocations here.
            [[nodiscard]] ExprNode LowerElement( const struct JsxElement& Element );
            [[nodiscard]] ExprNode LowerFragment( const struct JsxFragment& Fragment );
            [[nodiscard]] ExprNode LowerText( const struct JsxText& Text );

            AstContext& Context;
        };

    }

}
