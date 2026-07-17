#include "Volt/Driver/Driver.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

void PrintUsage ( std::ostream &Out )
{
    Out << "usage: Volt [--print] <file.vl|file.vlx> [more...]\n"
        << "       Volt [--print] --circuit <Project.vl>\n";
}

} // namespace

// Front CLI: hand the requested files (or a whole circuit) to the Driver, which
// parses and runs the sema passes across a thread pool, then report diagnostics.
int main ( int ArgCount, char **ArgValues )
{
    std::vector<std::string> Files;
    bool bCircuit = false;
    bool bPrint   = false;

    for ( int Index = 1; Index < ArgCount; ++Index )
    {
        const std::string_view Arg = ArgValues[Index];
        if ( Arg == "--circuit" )
        {
            bCircuit = true;
        }
        else if ( Arg == "--print" )
        {
            bPrint = true;
        }
        else if ( Arg == "-h" || Arg == "--help" )
        {
            PrintUsage( std::cout );
            return 0;
        }
        else
        {
            Files.emplace_back( Arg );
        }
    }

    if ( Files.empty() )
    {
        PrintUsage( std::cerr );
        return 2;
    }

    Volt::Driver::Driver Driver;
    Volt::Driver::CompileResult Result = bCircuit ? Driver.CompileCircuit( Files.front() ) : Driver.CompileFiles( Files );

    if ( bPrint )
    {
        Driver.PrintUnits( std::cout );
    }

    Driver.RenderDiagnostics( std::cerr );

    std::cerr << "[Volt] " << Result.Files << " file(s), " << Result.Errors << " error(s)";
    if ( bCircuit )
    {
        std::cerr << ", " << Driver.Graph().ModuleCount() << " module(s)";
        if ( Result.bCycle )
        {
            std::cerr << ", dependency cycle";
        }
    }
    std::cerr << '\n';

    return Driver.HasErrors() ? 1 : 0;
}
