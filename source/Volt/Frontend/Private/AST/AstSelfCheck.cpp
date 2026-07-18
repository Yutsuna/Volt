// Compile-time + smoke check for the manifest-generated AST and the reflective
// printer. Instantiates every category variant and prints a small tree so the
// header-only machinery is actually exercised under -Werror.

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstPrinter.hpp"

#include <sstream>
#include <string>
#include <variant>

namespace Volt
{

namespace Frontend
{

    namespace
    {

        static_assert( std::variant_size_v<ExprNode> >= 2 );
        static_assert( std::variant_size_v<StmtNode> >= 2 );
        static_assert( std::variant_size_v<DeclNode> >= 2 );
        static_assert( std::variant_size_v<TypeNode> >= 2 );

        // Every node reflects all of its fields except the leading Loc. A
        // field added to a struct but forgotten in VOLT_FIELDS would silently
        // vanish from the printer / walkers — fail the build instead.
        // clang-format off
#define VOLT_CHECK_NODE( Name )                                                                                                                                          \
    static_assert( Meta::AggregateArity<Name>() == Meta::FieldCount<Name>() + 1,                                                                                         \
                   #Name ": VOLT_FIELDS is out of sync with the struct (every field except Loc must be listed)" );
#define VOLT_EXPR( Name ) VOLT_CHECK_NODE( Name )
#define VOLT_STMT( Name ) VOLT_CHECK_NODE( Name )
#define VOLT_DECL( Name ) VOLT_CHECK_NODE( Name )
#define VOLT_TYPE( Name ) VOLT_CHECK_NODE( Name )
#include "Volt/Frontend/AST/Nodes.inl"
#undef VOLT_CHECK_NODE
        // clang-format on

        // Kind aligns with the manifest order (monostate = None = 0).
        // Only nodes without SmallVec members are constexpr-constructible;
        // the rest are covered at runtime by BuildAndPrint below.
        static_assert( KindOf( ExprNode{ IntLiteral{} } ) == ExprKind::IntLiteral );
        static_assert( KindOf( StmtNode{ Return{} } ) == StmtKind::Return );

        [[maybe_unused]] std::string BuildAndPrint ( Core::StringInterner &Interner, Core::FileId File )
        {
            AstContext Ctx{ Interner, File };

            // 1 + 2
            const ExprId One = Ctx.Add( IntLiteral{ {}, Interner.Intern( "1" ) } );
            const ExprId Two = Ctx.Add( IntLiteral{ {}, Interner.Intern( "2" ) } );

            Binary Sum;
            Sum.Op           = TokenKind::Plus;
            Sum.Lhs          = One;
            Sum.Rhs          = Two;
            const ExprId Add = Ctx.Add( ExprNode{ Sum } );

            Return Ret;
            Ret.Value            = Add;
            const StmtId RetStmt = Ctx.Add( StmtNode{ Ret } );

            Method M;
            M.Name       = Interner.Intern( "answer" );
            M.ReturnType = Ctx.Add( TypeNode{ TypeRef{ {}, { Interner.Intern( "Int32" ) }, {} } } );
            M.Body.PushBack( RetStmt );
            const DeclId Def = Ctx.Add( DeclNode{ M } );

            Ctx.TopDecls.push_back( Def );

            std::ostringstream Out;
            AstPrinter Printer{ Ctx, Out };
            Printer.PrintFile();
            return Out.str();
        }

    } // namespace

} // namespace Frontend

} // namespace Volt
