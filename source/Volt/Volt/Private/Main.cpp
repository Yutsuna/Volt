#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Diagnostics/SourceManager.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstPrinter.hpp"
#include "Volt/Frontend/AST/JsxLowering.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

[[nodiscard]] bool ReadFile ( const std::string &Path, std::string &Out )
{
    std::ifstream Stream( Path, std::ios::binary );
    if ( !Stream )
    {
        return false;
    }
    std::ostringstream Buffer;
    Buffer << Stream.rdbuf();
    Out = Buffer.str();
    return true;
}

[[nodiscard]] bool HasExtension ( std::string_view Path, std::string_view Suffix )
{
    return Path.size() >= Suffix.size() && Path.substr( Path.size() - Suffix.size() ) == Suffix;
}

} // namespace

// Minimal front-end driver: read a source file, parse it, lower its JSX to
// `Volt::JSX` calls, and print the resulting AST. The full circuit driver
// (parallel module graph) arrives in a later phase.
int main ( int ArgCount, char **ArgValues )
{
    if ( ArgCount < 2 )
    {
        std::cerr << "usage: Volt <file.vl|file.vlx>\n";
        return 2;
    }

    const std::string Path = ArgValues[1];
    std::string Text;
    if ( !ReadFile( Path, Text ) )
    {
        std::cerr << "error: cannot read '" << Path << "'\n";
        return 2;
    }

    Volt::Core::SourceManager Sources;
    Volt::Core::StringInterner Interner;
    Volt::Core::DiagEngine Diagnostics;

    const Volt::Core::FileId File   = Sources.AddFile( Path, Text );
    Volt::Core::DiagEngine::Bag Bag = Diagnostics.MakeBag();

    Volt::Frontend::Lexer Lexer( File, Text, Interner, Bag );
    std::vector<Volt::Frontend::Token> Tokens = Lexer.Tokenize();

    Volt::Frontend::AstContext Context( Interner, File );
    Volt::Frontend::Parser Parser( std::move( Tokens ), Context, Bag, Text );
    if ( HasExtension( Path, ".vlx" ) )
    {
        Parser.ParseComponentFile();
    }
    else
    {
        Parser.ParseFile();
    }

    const std::size_t Lowered = Volt::Frontend::JsxLowering( Context ).Run();

    Volt::Frontend::AstPrinter Printer( Context, std::cout );
    Printer.PrintFile();

    Diagnostics.Merge( std::move( Bag ) );
    Diagnostics.Render( Sources, std::cerr );

    if ( Lowered > 0 )
    {
        std::cerr << "[Volt] lowered " << Lowered << " JSX node(s)\n";
    }

    return Diagnostics.HasErrors() ? 1 : 0;
}
