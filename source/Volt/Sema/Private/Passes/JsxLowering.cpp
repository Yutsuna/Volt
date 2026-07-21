// JsxLowering — order 20. Rewrites every JSX node into ordinary calls onto
// the `Volt::JSX` runtime facade, so the tree is JSX-free before type
// checking and codegen:
//
//   <div class="x">child</div>  ->  Volt::JSX.create_element("div",
//                                       { "class" => "x" }, [ child ])
//   <>a b</>                     ->  Volt::JSX.fragment([ a, b ])
//   plain text                   ->  Volt::JSX.text("...")
//
// Interpolation children (`{expr}`) pass through unchanged inside the
// children array. Nodes are lowered in place: every reference is an ExprId,
// so replacing a slot rewrites the whole tree at once and order does not
// matter. The pass reports how many nodes it rewrote via PassStats.

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Jsx.hpp"
#include "Volt/Sema/Pass.hpp"

#include <cstddef>
#include <string_view>
#include <utility>

namespace Volt
{

namespace Sema
{

    namespace
    {

        using namespace Frontend;

        // The runtime facade the JSX tree is lowered onto. Names only —
        // never a Volt type — so the zero-hardcode guard stays satisfied.
        constexpr std::string_view FacadeRoot   = "Volt";
        constexpr std::string_view FacadeModule = "JSX";
        constexpr std::string_view CreateMethod = "create_element";
        constexpr std::string_view TextMethod   = "text";
        constexpr std::string_view FragmentMeth = "fragment";

        class JsxRewriter
        {

        public:

            explicit JsxRewriter ( AstContext &InContext ) : Context( InContext )
            {
            }

            /// Lower every JSX node currently in the context. Returns the
            /// number of nodes rewritten.
            std::size_t Run ()
            {
                // Only nodes present before lowering can be JSX; the calls we
                // append land past this bound and never need visiting.
                const std::size_t OriginalCount = Context.ExprCount();
                std::size_t Rewritten           = 0;

                for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
                {
                    const ExprId Id{ static_cast<ExprId::ValueType>( Index ) };

                    // Copy the node out of the arena first: the Lower*
                    // helpers append nodes and may reallocate it, so no
                    // reference into it may survive.
                    ExprNode Lowered;
                    switch ( KindOf( Context.Expr( Id ) ) )
                    {
                    case ExprKind::JsxElement:
                    {
                        const JsxElement Node = std::get<JsxElement>( Context.Expr( Id ) );
                        Lowered               = LowerElement( Node );
                        break;
                    }
                    case ExprKind::JsxFragment:
                    {
                        const JsxFragment Node = std::get<JsxFragment>( Context.Expr( Id ) );
                        Lowered                = LowerFragment( Node );
                        break;
                    }
                    case ExprKind::JsxText:
                    {
                        const JsxText Node = std::get<JsxText>( Context.Expr( Id ) );
                        Lowered            = LowerText( Node );
                        break;
                    }
                    default:
                        continue;
                    }

                    Context.Expr( Id ) = std::move( Lowered );
                    ++Rewritten;
                }

                return Rewritten;
            }

        private:

            /// `Volt::JSX.<Method>` — the member chain used as a call callee.
            [[nodiscard]] ExprId MakeFacade ( std::string_view Method, Core::SourceRange Loc )
            {
                Core::StringInterner &Strings = Context.Strings();

                Identifier RootNode;
                RootNode.Loc      = Loc;
                RootNode.Name     = Strings.Intern( FacadeRoot );
                const ExprId Root = Context.Add( ExprNode{ RootNode } );

                Frontend::Member ModuleNode;
                ModuleNode.Loc      = Loc;
                ModuleNode.Object   = Root;
                ModuleNode.Name     = Strings.Intern( FacadeModule );
                const ExprId Module = Context.Add( ExprNode{ ModuleNode } );

                Frontend::Member MethodNode;
                MethodNode.Loc    = Loc;
                MethodNode.Object = Module;
                MethodNode.Name   = Strings.Intern( Method );
                return Context.Add( ExprNode{ MethodNode } );
            }

            [[nodiscard]] ExprNode LowerElement ( const JsxElement &Element )
            {
                const Core::SourceRange Loc = Element.Loc;

                // Arg 0: the tag name as a string literal.
                StringLiteral TagLit;
                TagLit.Loc       = Loc;
                TagLit.Value     = Element.Tag;
                const ExprId Tag = Context.Add( ExprNode{ TagLit } );

                // Arg 1: `{ "name" => value, ... }` of the attributes.
                HashLit Attrs;
                Attrs.Loc = Loc;
                for ( std::size_t Index = 0; Index < Element.AttrNames.Size(); ++Index )
                {
                    StringLiteral Key;
                    Key.Loc   = Loc;
                    Key.Value = Element.AttrNames[Index];
                    Attrs.Keys.PushBack( Context.Add( ExprNode{ Key } ) );
                    Attrs.Values.PushBack( Element.AttrValues[Index] );
                }
                const ExprId AttrHash = Context.Add( ExprNode{ std::move( Attrs ) } );

                // Arg 2: `[ child, ... ]`; children keep their Ids and lower in place.
                ArrayLit Children;
                Children.Loc            = Loc;
                Children.Elements       = Element.Children;
                const ExprId ChildArray = Context.Add( ExprNode{ std::move( Children ) } );

                Call Node;
                Node.Loc    = Loc;
                Node.Callee = MakeFacade( CreateMethod, Loc );
                Node.Args.PushBack( Tag );
                Node.Args.PushBack( AttrHash );
                Node.Args.PushBack( ChildArray );
                Node.ArgNames.PushBack( Symbol{} );
                Node.ArgNames.PushBack( Symbol{} );
                Node.ArgNames.PushBack( Symbol{} );
                return ExprNode{ std::move( Node ) };
            }

            [[nodiscard]] ExprNode LowerFragment ( const JsxFragment &Fragment )
            {
                const Core::SourceRange Loc = Fragment.Loc;

                ArrayLit Children;
                Children.Loc            = Loc;
                Children.Elements       = Fragment.Children;
                const ExprId ChildArray = Context.Add( ExprNode{ std::move( Children ) } );

                Call Node;
                Node.Loc    = Loc;
                Node.Callee = MakeFacade( FragmentMeth, Loc );
                Node.Args.PushBack( ChildArray );
                Node.ArgNames.PushBack( Symbol{} );
                return ExprNode{ std::move( Node ) };
            }

            [[nodiscard]] ExprNode LowerText ( const JsxText &Text )
            {
                const Core::SourceRange Loc = Text.Loc;

                StringLiteral Lit;
                Lit.Loc              = Loc;
                Lit.Value            = Text.Text;
                const ExprId Literal = Context.Add( ExprNode{ Lit } );

                Call Node;
                Node.Loc    = Loc;
                Node.Callee = MakeFacade( TextMethod, Loc );
                Node.Args.PushBack( Literal );
                Node.ArgNames.PushBack( Symbol{} );
                return ExprNode{ std::move( Node ) };
            }

            AstContext &Context;
        };

    } // namespace

    // Order 20 — rewrite JSX nodes into `Volt::JSX` runtime calls. Must run
    // before type checking so later passes never see a JSX node.
    void JsxLowering ( PassContext &Context )
    {
        Context.Stats.JsxLowered += JsxRewriter( Context.Ast ).Run();
    }

} // namespace Sema

} // namespace Volt
