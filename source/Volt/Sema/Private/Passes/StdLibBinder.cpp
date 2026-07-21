// StdLibBinder.cpp — Order 11 pass: populates TypeStore from @[Primitive] and @[Literal] annotations.

#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Type.hpp"
#include "Volt/Sema/Pass.hpp"

#include <cstdlib>
#include <string_view>
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
            Frontend::ExprList Args;
        };

        template <typename DeclContainer> void ProcessDeclList ( PassContext &Context, const DeclContainer &Decls )
        {
            std::vector<PendingAnnotation> Pending;

            for ( const Frontend::DeclId Id : Decls )
            {
                if ( !Id.IsValid() )
                {
                    continue;
                }

                const Frontend::DeclNode &Node = Context.Ast.Decl( Id );

                if ( const auto *Anno = std::get_if<Frontend::Annotation>( &Node ) )
                {
                    Pending.push_back( PendingAnnotation{ Anno->Name, Anno->Args } );
                    continue;
                }

                if ( const auto *ModuleDecl = std::get_if<Frontend::Module>( &Node ) )
                {
                    ProcessDeclList( Context, ModuleDecl->Body );
                    Pending.clear();
                    continue;
                }

                if ( const auto *StructDecl = std::get_if<Frontend::Struct>( &Node ) )
                {
                    LayoutId ResultLayout{};
                    bool bHasPrimitive = false;
                    Symbol LiteralKindSymbol{};

                    for ( const auto &P : Pending )
                    {
                        const std::string_view AnnoName = Context.Ast.Text( P.Name );
                        if ( AnnoName == "Primitive" and P.Args.Size() >= 2 )
                        {
                            if ( const auto Spelling = Frontend::AsStringText( Context.Ast, P.Args[0] ) )
                            {
                                std::uint32_t Bits = 0;
                                if ( const auto *IntLit = std::get_if<Frontend::IntLiteral>( &Context.Ast.Expr( P.Args[1] ) ) )
                                {
                                    const std::string_view RawBits = Context.Ast.Text( IntLit->Raw );
                                    Bits = static_cast<std::uint32_t>( std::strtoul( RawBits.data(), nullptr, 10 ) );
                                }
                                const Symbol SpellingSym = Context.Ast.Strings().Intern( *Spelling );
                                ResultLayout             = Context.Types.AddPrimitive( SpellingSym, Bits );
                                Context.Types.Bind( StructDecl->Name, ResultLayout );
                                bHasPrimitive = true;
                            }
                        }
                        else if ( AnnoName == "Literal" and P.Args.Size() >= 1 )
                        {
                            if ( const auto *IdExpr = std::get_if<Frontend::Identifier>( &Context.Ast.Expr( P.Args[0] ) ) )
                            {
                                LiteralKindSymbol = IdExpr->Name;
                            }
                        }
                    }

                    if ( !bHasPrimitive )
                    {
                        Aggregate Agg;
                        for ( const Frontend::DeclId BodyId : StructDecl->Body )
                        {
                            if ( const auto *FieldDecl = std::get_if<Frontend::Field>( &Context.Ast.Decl( BodyId ) ) )
                            {
                                LayoutId FieldLayoutId{};
                                if ( FieldDecl->DeclType.IsValid() )
                                {
                                    if ( const auto *TypeRefNode =
                                             std::get_if<Frontend::TypeRef>( &Context.Ast.Type( FieldDecl->DeclType ) ) )
                                    {
                                        if ( TypeRefNode->Path.Size() > 0 )
                                        {
                                            const Symbol TypeSym = TypeRefNode->Path[TypeRefNode->Path.Size() - 1];
                                            if ( const auto OptLayout = Context.Types.Lookup( TypeSym ) )
                                            {
                                                FieldLayoutId = *OptLayout;
                                            }
                                        }
                                    }
                                }
                                Agg.Fields.PushBack( FieldLayout{ FieldDecl->Name, FieldLayoutId } );
                            }
                        }
                        ResultLayout = Context.Types.AddAggregate( std::move( Agg ) );
                        Context.Types.Bind( StructDecl->Name, ResultLayout );
                    }

                    if ( LiteralKindSymbol.IsValid() and ResultLayout.IsValid() )
                    {
                        Context.Types.BindLiteral( LiteralKindSymbol, ResultLayout );
                    }

                    Pending.clear();
                    continue;
                }

                Pending.clear();
            }
        }

    } // namespace

    void StdLibBinder ( PassContext &Context )
    {
        ProcessDeclList( Context, Context.Ast.TopDecls );
    }

} // namespace Sema

} // namespace Volt
