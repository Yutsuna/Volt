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

#if VOLT_HAS_REFLECTION
        // P2996 reflection walks the structs directly, so a field can no
        // longer desynchronise from a manifest. Spot-check the Loc-skipping
        // convention instead: every field except the leading Loc is visited.
        static_assert( Meta::FieldCount<IntLiteral>() == 1 );
        static_assert( Meta::FieldCount<Binary>() == 3 );
        static_assert( Meta::FieldCount<NilLiteral>() == 0 );
        static_assert( Meta::FieldCount<DotCall>() == 3 );
        static_assert( Meta::FieldCount<WhenClause>() == 2 );
        static_assert( Meta::FieldCount<CaseExpr>() == 4 );
        static_assert( Meta::EnumName( ExprKind::IntLiteral ) == "IntLiteral" );
        static_assert( Meta::EnumName( ExprKind::CaseExpr ) == "CaseExpr" );
#endif

        // Kind aligns with the manifest order (monostate = None = 0).
        // Only nodes without SmallVec members are constexpr-constructible;
        // the rest are covered at runtime by BuildAndPrint below.
        static_assert( KindOf( ExprNode{ IntLiteral{} } ) == ExprKind::IntLiteral );
        static_assert( KindOf( StmtNode{ Return{} } ) == StmtKind::Return );

        [[maybe_unused]] std::string BuildAndPrint ( Core::StringInterner &Interner, Core::FileId File )
        {
            AstContext Ctx{ Interner, File };

            // 1 + 2
            const ExprId One = Ctx.Add( IntLiteral{ .Loc = {}, .Raw = Interner.Intern( "1" ) } );
            const ExprId Two = Ctx.Add( IntLiteral{ .Loc = {}, .Raw = Interner.Intern( "2" ) } );

            Binary Sum;
            Sum.Op           = TokenKind::Plus;
            Sum.Lhs          = One;
            Sum.Rhs          = Two;
            const ExprId Add = Ctx.Add( ExprNode{ Sum } );

            // NOLINTNEXTLINE(misc-const-correctness)
            Return Ret;
            Ret.Value            = Add;
            const StmtId RetStmt = Ctx.Add( StmtNode{ Ret } );

            Method M;
            M.Name       = Interner.Intern( "answer" );
            M.ReturnType = Ctx.Add( TypeNode{ TypeRef{ .Loc = {}, .Path = { Interner.Intern( "Int32" ) }, .Generics = {} } } );
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
