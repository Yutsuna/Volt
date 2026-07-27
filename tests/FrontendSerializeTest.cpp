// FrontendSerializeTest — round-trip proof for AstContext::SerializeCache/
// DeserializeCache (issue #61, Phase 2c): a real lexed + parsed AstContext,
// not a hand-built one, so every ExprNode/StmtNode/DeclNode/TypeNode
// alternative the parser actually produces gets exercised through the
// generic Meta::Reflected path. Same standalone-executable shape as
// CoreSerializeTest/SemaSerializeTest.

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"

#include <print>

namespace
{

using namespace Volt;
using namespace Volt::Frontend;

int Failures = 0;

void Check ( bool bCondition, const char *Label )
{
    if ( not bCondition )
    {
        std::println( stderr, "FAIL: {}", Label );
        ++Failures;
    }
}

constexpr std::string_view Source = R"(
def add( a : Int32, b : Int32 ) -> Int32
  x = a + b
  return x
end

class Point
  x : Int32
  y : Int32

  def length() -> Int32
    return @x + @y
  end
end
)";

} // namespace

int main ()
{
    Core::StringInterner Interner;
    Core::DiagEngine::Bag Bag;

    Lexer Lex{ Core::FileId{}, Source, Interner, Bag };
    std::vector<Token> Tokens = Lex.Tokenize();

    AstContext Original{ Interner, Core::FileId{} };
    Parser P{ Tokens, Original, Bag, Source };
    P.ParseFile();

    Check( Bag.Errors() == 0, "the fixture parses with no diagnostics" );
    Check( Original.TopDecls.size() == 2, "fixture has two top-level declarations" );
    Check( Original.ExprCount() > 0, "fixture parsed at least one expression" );

    Meta::Writer W;
    Original.SerializeCache( W );

    Meta::Reader R{ W.Data() };
    AstContext Restored{ Interner, Core::FileId{} };
    Check( Restored.DeserializeCache( R ), "AstContext deserialize reports success" );
    Check( not R.Failed(), "AstContext reader not failed" );

    Check( Restored.ExprCount() == Original.ExprCount(), "ExprCount round-trips" );
    Check( Restored.StmtCount() == Original.StmtCount(), "StmtCount round-trips" );
    Check( Restored.DeclCount() == Original.DeclCount(), "DeclCount round-trips" );
    Check( Restored.TypeCount() == Original.TypeCount(), "TypeCount round-trips" );
    Check( Restored.ParamCount() == Original.ParamCount(), "ParamCount round-trips" );
    Check( Restored.TopDecls.size() == Original.TopDecls.size(), "TopDecls round-trips" );

    // Structural spot-check, not a byte diff: the second top-level decl
    // ("Point") keeps its own name and field/method members after the round
    // trip, reached the same way AstPrinter would (NodeName + std::visit).
    const DeclId PointId = Restored.TopDecls[1];
    std::visit(
        [&] ( const auto &Node )
        {
            using T = std::remove_cvref_t<decltype( Node )>;
            if constexpr ( std::same_as<T, Class> )
            {
                Check( Restored.Text( Node.Name ) == "Point", "restored Class keeps its Name" );
                Check( Node.Body.Size() == 3, "restored Class keeps its 3 body decls (2 fields + 1 method)" );
            }
            else
            {
                Check( false, "second top-level decl is a Class" );
            }
        },
        Restored.Decl( PointId ) );

    if ( Failures == 0 )
    {
        std::println( "FrontendSerializeTest: all checks passed" );
        return 0;
    }
    std::println( stderr, "FrontendSerializeTest: {} check(s) failed", Failures );
    return 1;
}
