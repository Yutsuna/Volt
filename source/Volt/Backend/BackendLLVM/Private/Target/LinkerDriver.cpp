// LinkerDriver.cpp — see LinkerDriver.hpp.

#include "Target/LinkerDriver.hpp"

#include "Core/DiagnosticSink.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/Program.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

std::vector<std::string> Volt::Backend::Llvm::LinkerDriver::ExternLibraries () const
{
    std::vector<std::string> Libraries;
    auto Collect = [&] ( const Sema::Member &Entry )
    {
        if ( not Entry.ExternLib.IsValid() )
        {
            return;
        }
        const std::string Name( Services->Build->Types->Text( Entry.ExternLib ) );
        if ( Name.empty() )
        {
            return;
        }
        for ( const std::string &Seen : Libraries )
        {
            if ( Seen == Name )
            {
                return;
            }
        }
        Libraries.push_back( Name );
    };

    Sema::TypeStore &Store = *Services->Build->Types;
    for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
    {
        const Sema::NominalId Id{ static_cast<Sema::NominalId::ValueType>( Index ) };
        for ( const Sema::Member &Entry : Store.Type( Id ).Members )
        {
            Collect( Entry );
        }
    }
    for ( const Sema::Member &Entry : Store.FreeFunctions() )
    {
        Collect( Entry );
    }

    return Libraries;
}

namespace
{

// Shared by LinkExecutable and LinkSharedLibrary: find the C driver, then run it
// over a caller-supplied Args vector that already has "-o" and every input in
// place. A driver, not the raw linker: it supplies the CRT objects, the dynamic
// linker path and the default libc that a bare `ld`/`mold`/`lld` invocation
// would otherwise have to hand-derive per platform.
[[nodiscard]] llvm::ErrorOr<std::string> FindLinkerDriver ()
{
    llvm::ErrorOr<std::string> Driver = llvm::sys::findProgramByName( "cc" );
    if ( not Driver )
    {
        Driver = llvm::sys::findProgramByName( "clang" );
    }
    if ( not Driver )
    {
        Driver = llvm::sys::findProgramByName( "gcc" );
    }
    return Driver;
}

// "libc" -> "-lc": the conventional stripping of a "lib" prefix a linker's own
// `-l` flag already assumes; a name with no such prefix (an external C library
// that spells itself, e.g. "SDL2") passes through as `-lSDL2`.
[[nodiscard]] std::string LibraryFlag ( const std::string &Library )
{
    if ( Library == "volt" )
    {
        return std::string{};
    }
    constexpr std::string_view Prefix = "lib";
    const std::string_view Spelling =
        Library.starts_with( Prefix ) ? std::string_view( Library ).substr( Prefix.size() ) : std::string_view( Library );
    return Spelling.empty() ? std::string{} : "-l" + std::string( Spelling );
}

// Prefer mold, then LLD — the same preference the compiler's own build encodes —
// falling through to whatever the driver picks by default when neither is on
// PATH.
void AppendPreferredLinker ( std::vector<std::string> &Args )
{
    if ( llvm::sys::findProgramByName( "mold" ) )
    {
        Args.emplace_back( "-fuse-ld=mold" );
    }
    else if ( llvm::sys::findProgramByName( "ld.lld" ) )
    {
        Args.emplace_back( "-fuse-ld=lld" );
    }
}

[[nodiscard]] int
RunDriver ( const std::string &Driver, const std::vector<std::string> &Args, std::string &OutError, bool &bFailed )
{
    std::vector<llvm::StringRef> ArgRefs;
    ArgRefs.reserve( Args.size() );
    for ( const std::string &Arg : Args )
    {
        ArgRefs.emplace_back( Arg );
    }
    return llvm::sys::ExecuteAndWait( Driver, ArgRefs, std::nullopt, {}, 0, 0, &OutError, &bFailed );
}

} // namespace

bool Volt::Backend::Llvm::LinkerDriver::LinkExecutable ( std::string_view ObjectPath, std::string_view OutputPath )
{
    const llvm::ErrorOr<std::string> Driver = FindLinkerDriver();
    if ( not Driver )
    {
        static_cast<void>(
            Services->Diag->Fail( "llvm: no C compiler driver ('cc', 'clang', 'gcc') found on PATH to link with" ) );
        return false;
    }

    std::vector<std::string> Args{ *Driver, "-o", std::string( OutputPath ), std::string( ObjectPath ) };

    // The precompiled stdlib archive/.so (issue #61's native-artifact cache),
    // when the caller set one: right after the primary object, so a static
    // archive among them can still resolve the symbols that object references — a
    // traditional single-pass linker only searches archives that come *after* the
    // reference.
    for ( const std::string &Extra : Services->Options->ExtraLinkInputs )
    {
        Args.push_back( Extra );
    }

    AppendPreferredLinker( Args );

    for ( const std::string &Library : ExternLibraries() )
    {
        if ( std::string Flag = LibraryFlag( Library ); not Flag.empty() )
        {
            Args.emplace_back( std::move( Flag ) );
        }
    }

    std::string ErrorMessage;
    bool bExecutionFailed = false;
    const int Result      = RunDriver( *Driver, Args, ErrorMessage, bExecutionFailed );
    if ( bExecutionFailed or Result != 0 )
    {
        static_cast<void>( Services->Diag->Fail( "llvm: link failed (" + *Driver + ", exit " + std::to_string( Result ) +
                                                 "): " + ErrorMessage ) );
        return false;
    }

    return true;
}

bool Volt::Backend::Llvm::LinkerDriver::LinkSharedLibrary ( std::string_view ObjectPath, std::string_view OutputPath )
{
    const llvm::ErrorOr<std::string> Driver = FindLinkerDriver();
    if ( not Driver )
    {
        static_cast<void>(
            Services->Diag->Fail( "llvm: no C compiler driver ('cc', 'clang', 'gcc') found on PATH to link with" ) );
        return false;
    }

    // `-shared -fPIC`: InitTarget already selected Reloc::PIC_ for every build, so
    // the object itself needs no different codegen — only the driver's final link
    // mode changes.
    std::vector<std::string> Args{ *Driver, "-shared", "-fPIC", "-o", std::string( OutputPath ), std::string( ObjectPath ) };

    for ( const std::string &Extra : Services->Options->ExtraLinkInputs )
    {
        Args.push_back( Extra );
    }

    AppendPreferredLinker( Args );

    for ( const std::string &Library : ExternLibraries() )
    {
        if ( std::string Flag = LibraryFlag( Library ); not Flag.empty() )
        {
            Args.emplace_back( std::move( Flag ) );
        }
    }

    std::string ErrorMessage;
    bool bExecutionFailed = false;
    const int Result      = RunDriver( *Driver, Args, ErrorMessage, bExecutionFailed );
    if ( bExecutionFailed or Result != 0 )
    {
        static_cast<void>( Services->Diag->Fail( "llvm: shared-object link failed (" + *Driver + ", exit " +
                                                 std::to_string( Result ) + "): " + ErrorMessage ) );
        return false;
    }

    return true;
}
