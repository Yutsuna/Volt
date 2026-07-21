// TypeBinder.cpp — the serial cross-unit seam that turns `@[Primitive(...)]`
// and `@[Literal(...)]` into TypeStore bindings.
//
// Zero-hardcode lives or dies here: this file must never mention `Int32`,
// `String` or any other Volt type. It reads the *spelling* out of the
// annotation ("i32") and the literal kind the type claims ("IntLiteral"),
// and binds the two. Which Volt type wraps a bare `10` is decided entirely
// by which struct carries `@[Literal( IntLiteral )]` in source/Lib/.

#include "Volt/Sema/Layout/TypeBinder.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Type.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Volt
{

namespace Sema
{

    namespace
    {

        struct PendingAnnotation
        {

            Symbol Name;
            Frontend::ExprList Args{};
            Core::SourceRange Loc;
        };

        // The bit width of `@[Primitive( "i32", 32 )]`'s second argument.
        // Absent or malformed means "width unknown" (0), which is legal: a
        // pointer-shaped primitive may leave it to the target layout.
        [[nodiscard]] std::uint32_t ReadBits ( const Frontend::AstContext &Ast, Frontend::ExprId Id )
        {
            const auto *Literal = std::get_if<Frontend::IntLiteral>( &Ast.Expr( Id ) );
            if ( Literal == nullptr )
            {
                return 0;
            }

            const std::string_view Raw = Ast.Text( Literal->Raw );
            std::uint32_t Bits         = 0;
            // from_chars over the interned view: Text() is not NUL-terminated,
            // so strtoul would read past the end of the string block.
            std::from_chars( Raw.data(), Raw.data() + Raw.size(), Bits );
            return Bits;
        }

        // The layout of a type annotation's field, when the field's type name
        // is already bound. An unresolved field keeps an invalid LayoutId
        // rather than inventing one — full resolution is the TypeChecker's job.
        [[nodiscard]] LayoutId FieldLayoutOf ( const Frontend::AstContext &Ast, const TypeStore &Store, Frontend::TypeId Id )
        {
            if ( not Id.IsValid() )
            {
                return LayoutId{};
            }
            const auto *Ref = std::get_if<Frontend::TypeRef>( &Ast.Type( Id ) );
            if ( Ref == nullptr or Ref->Path.Size() == 0 )
            {
                return LayoutId{};
            }
            const Symbol Last = Ref->Path[Ref->Path.Size() - 1];
            if ( const auto Found = Store.LookupType( Ast.Text( Last ) ) )
            {
                return Store.Type( *Found ).Layout;
            }
            return LayoutId{};
        }

        struct Binder
        {

            const Frontend::AstContext &Ast;
            TypeStore &Store;
            Core::DiagEngine::Bag &Diags;
            std::uint32_t Unit = 0;
            std::size_t Bound  = 0;

            void Report ( Core::ESeverity Severity, Core::SourceRange Loc, const std::string &Message )
            {
                Diags.Report( Core::Diagnostic{ .Severity = Severity, .Range = Loc, .Message = Message, .Notes = {} } );
            }

            // The aggregate a struct collapses to when it has no
            // `@[Primitive]`: its fields in declaration order.
            [[nodiscard]] LayoutId AggregateOf ( const Frontend::DeclList &Body )
            {
                Aggregate Agg;
                for ( const Frontend::DeclId Id : Body )
                {
                    if ( const auto *Field = std::get_if<Frontend::Field>( &Ast.Decl( Id ) ) )
                    {
                        Agg.Fields.PushBack( FieldLayout{ Store.Intern( Ast.Text( Field->Name ) ),
                                                          FieldLayoutOf( Ast, Store, Field->DeclType ) } );
                    }
                }
                return Store.AddAggregate( std::move( Agg ) );
            }

            // Bind one declaration that carries `Pending` annotations.
            void BindType ( std::string_view Name,
                            Frontend::DeclId Decl,
                            const Frontend::DeclList &Body,
                            const std::vector<PendingAnnotation> &Pending )
            {
                // The type exists as an identity first; its layout is an
                // attribute that may stay unresolved (generics, aggregates
                // whose fields are not bound yet). Type checking never needs it.
                const NominalId Id = Store.DeclareType( Name, Unit, Decl );
                ++Bound;

                LayoutId Layout;
                std::string_view LiteralKind;
                Core::SourceRange LiteralLoc;

                for ( const PendingAnnotation &Anno : Pending )
                {
                    const std::string_view AnnoName = Ast.Text( Anno.Name );

                    if ( AnnoName == "Primitive" and Anno.Args.Size() >= 1 )
                    {
                        const auto Spelling = Frontend::AsStringText( Ast, Anno.Args[0] );
                        if ( not Spelling )
                        {
                            Report( Core::ESeverity::Error, Anno.Loc,
                                    "@[Primitive] expects a layout spelling string, e.g. @[Primitive( \"i32\", 32 )]" );
                            continue;
                        }
                        const std::uint32_t Bits = Anno.Args.Size() >= 2 ? ReadBits( Ast, Anno.Args[1] ) : 0;
                        Layout                   = Store.AddPrimitive( Store.Intern( *Spelling ), Bits );
                    }
                    else if ( AnnoName == "Literal" )
                    {
                        const auto *Kind =
                            Anno.Args.Size() >= 1 ? std::get_if<Frontend::Identifier>( &Ast.Expr( Anno.Args[0] ) ) : nullptr;
                        if ( Kind == nullptr )
                        {
                            Report( Core::ESeverity::Error, Anno.Loc,
                                    "@[Literal] expects a literal kind, e.g. @[Literal( IntLiteral )]" );
                            continue;
                        }
                        LiteralKind = Ast.Text( Kind->Name );
                        LiteralLoc  = Anno.Loc;
                    }
                }

                // Without @[Primitive] the layout is structural, and only
                // computable for a non-generic whose field types are already
                // bound. Leaving it invalid is correct, not a failure.
                if ( not Layout.IsValid() and not Body.IsEmpty() )
                {
                    Layout = AggregateOf( Body );
                }
                if ( Layout.IsValid() )
                {
                    Store.AttachLayout( Id, Layout );
                }

                if ( LiteralKind.empty() )
                {
                    return;
                }

                // Two types claiming the same literal kind would make the type
                // of `10` depend on stdlib file order. Refuse instead.
                if ( not Store.BindLiteral( LiteralKind, Id ) )
                {
                    Report( Core::ESeverity::Error, LiteralLoc,
                            "literal kind '" + std::string{ LiteralKind } +
                                "' is already claimed by another type; only one type may wrap it" );
                }
            }

            // Templated over the container: top-level decls are a
            // std::vector, nested bodies a DeclList.
            template <typename DeclContainer> void Walk ( const DeclContainer &Decls )
            {
                std::vector<PendingAnnotation> Pending;

                for ( const Frontend::DeclId Id : Decls )
                {
                    if ( not Id.IsValid() )
                    {
                        continue;
                    }

                    const Frontend::DeclNode &Node = Ast.Decl( Id );

                    if ( const auto *Anno = std::get_if<Frontend::Annotation>( &Node ) )
                    {
                        Pending.push_back( PendingAnnotation{ Anno->Name, Anno->Args, Anno->Loc } );
                        continue;
                    }

                    if ( const auto *Nested = std::get_if<Frontend::Module>( &Node ) )
                    {
                        Walk( Nested->Body );
                    }
                    // One overload per declaration that introduces a type;
                    // everything else falls through the catch-all. No switch
                    // over DeclKind, so a new category cannot desync this.
                    std::visit(
                        Meta::Overloaded{
                            [&] ( const Frontend::Module &Nested ) { Walk( Nested.Body ); },
                            [&] ( const Frontend::Struct &Type ) { BindType( Ast.Text( Type.Name ), Id, Type.Body, Pending ); },
                            [&] ( const Frontend::Class &Type ) { BindType( Ast.Text( Type.Name ), Id, Type.Body, Pending ); },
                            [] ( const auto & ) {},
                        },
                        Node );

                    // Annotations bind to the declaration they precede, and
                    // never carry past it.
                    Pending.clear();
                }
            }
        };

    } // namespace

    std::size_t
    BindUnitTypes ( const Frontend::AstContext &Ast, std::uint32_t Unit, TypeStore &Store, Core::DiagEngine::Bag &Diags )
    {
        Binder Bind{ .Ast = Ast, .Store = Store, .Diags = Diags, .Unit = Unit };
        Bind.Walk( Ast.TopDecls );
        return Bind.Bound;
    }

} // namespace Sema

} // namespace Volt
