#include "Volt/Frontend/AST/JsxLowering.hpp"

#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Jsx.hpp"

#include <cstddef>
#include <string_view>
#include <utility>
#include <variant>

namespace Volt
{

    namespace Frontend
    {

        namespace
        {

            // The runtime facade the JSX tree is lowered onto. Names only —
            // never a Volt type — so the zero-hardcode guard stays satisfied.
            constexpr std::string_view FacadeRoot   = "Volt";
            constexpr std::string_view FacadeModule = "JSX";
            constexpr std::string_view CreateMethod = "create_element";
            constexpr std::string_view TextMethod   = "text";
            constexpr std::string_view FragmentMeth = "fragment";

        }

        ExprId JsxLowering::MakeFacade( std::string_view Method, Core::SourceRange Loc )
        {
            Core::StringInterner& Strings = Context.Strings();

            Identifier RootNode;
            RootNode.Loc      = Loc;
            RootNode.Name     = Strings.Intern( FacadeRoot );
            const ExprId Root = Context.Add( ExprNode{ RootNode } );

            Member ModuleNode;
            ModuleNode.Loc      = Loc;
            ModuleNode.Object   = Root;
            ModuleNode.Name     = Strings.Intern( FacadeModule );
            const ExprId Module = Context.Add( ExprNode{ ModuleNode } );

            Member MethodNode;
            MethodNode.Loc    = Loc;
            MethodNode.Object = Module;
            MethodNode.Name   = Strings.Intern( Method );
            return Context.Add( ExprNode{ MethodNode } );
        }

        ExprNode JsxLowering::LowerElement( const JsxElement& Element )
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

        ExprNode JsxLowering::LowerFragment( const JsxFragment& Fragment )
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

        ExprNode JsxLowering::LowerText( const JsxText& Text )
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

        std::size_t JsxLowering::Run()
        {
            // Only nodes present before lowering can be JSX; the calls we append
            // land past this bound and never need visiting. References are Ids,
            // so an in-place rewrite fixes every parent at once, order-free.
            const std::size_t OriginalCount = Context.ExprCount();
            std::size_t       Rewritten     = 0;

            for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
            {
                const ExprId Id{ static_cast<ExprId::ValueType>( Index ) };

                // Copy the node out of the arena first: the Lower* helpers append
                // nodes and may reallocate it, so no reference into it may survive.
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

    }

}
