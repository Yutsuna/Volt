// Linker.cpp — drives the system linker over the object Optimizer.cpp
// emitted (llvm.md, "Optimisation & emission": "mold when available, LLD
// fallback"). The link itself goes through a C compiler driver (`cc`), not a
// raw invocation of `ld`: only the driver knows this host's CRT startup
// objects, dynamic linker path and default libc, and reinventing that table
// here would be exactly the kind of guess rules/zero-hardcode.md's spirit
// (never bake in what belongs to the platform toolchain) warns against.

#include "LlvmState.hpp"

#include <llvm/Support/Program.h>

#include <string>
#include <vector>

std::vector<std::string> Volt::Backend::Llvm::LlvmBackend::State::ExternLibraries () const
{
    std::vector<std::string> Libraries;
    auto Collect = [&] ( const Sema::Member &Entry )
    {
        if ( not Entry.ExternLib.IsValid() )
        {
            return;
        }
        const std::string Name( Build->Types->Text( Entry.ExternLib ) );
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

    Sema::TypeStore &Store = *Build->Types;
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

// Shared by LinkExecutable and LinkSharedLibrary: find the C driver, then
// run it over a caller-supplied Args vector that already has "-o" and every
// input in place. A driver, not the raw linker: it supplies the CRT objects,
// the dynamic linker path and the default libc that a bare `ld`/`mold`/`lld`
// invocation would otherwise have to hand-derive per platform.
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

// "libc" -> "-lc": the conventional stripping of a "lib" prefix a linker's
// own `-l` flag already assumes; a name with no such prefix (an external C
// library that spells itself, e.g. "SDL2") passes through as `-lSDL2`.
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

} // namespace

bool Volt::Backend::Llvm::LlvmBackend::State::LinkExecutable ( std::string_view ObjectPath, std::string_view OutputPath )
{
    const llvm::ErrorOr<std::string> Driver = FindLinkerDriver();
    if ( not Driver )
    {
        static_cast<void>( Fail( "llvm: no C compiler driver ('cc', 'clang', 'gcc') found on PATH to link with" ) );
        return false;
    }

    std::vector<std::string> Args{ *Driver, "-o", std::string( OutputPath ), std::string( ObjectPath ) };

    // The precompiled stdlib archive/.so (issue #61's native-artifact
    // cache), when the caller set one: right after the primary object, so a
    // static archive among them can still resolve the symbols that object
    // references — a traditional single-pass linker only searches archives
    // that come *after* the reference.
    for ( const std::string &Extra : Options.ExtraLinkInputs )
    {
        Args.push_back( Extra );
    }

    // Prefer mold, then LLD — the same preference cmake/VoltOptions.cmake
    // encodes for the compiler's own build — falling through to whatever the
    // driver picks by default when neither is on PATH.
    if ( llvm::sys::findProgramByName( "mold" ) )
    {
        Args.emplace_back( "-fuse-ld=mold" );
    }
    else if ( llvm::sys::findProgramByName( "ld.lld" ) )
    {
        Args.emplace_back( "-fuse-ld=lld" );
    }

    for ( const std::string &Library : ExternLibraries() )
    {
        if ( std::string Flag = LibraryFlag( Library ); not Flag.empty() )
        {
            Args.emplace_back( std::move( Flag ) );
        }
    }

    std::vector<llvm::StringRef> ArgRefs;
    ArgRefs.reserve( Args.size() );
    for ( const std::string &Arg : Args )
    {
        ArgRefs.emplace_back( Arg );
    }

    std::string ErrorMessage;
    bool bExecutionFailed = false;
    const int Result = llvm::sys::ExecuteAndWait( *Driver, ArgRefs, std::nullopt, {}, 0, 0, &ErrorMessage, &bExecutionFailed );
    if ( bExecutionFailed or Result != 0 )
    {
        static_cast<void>(
            Fail( "llvm: link failed (" + *Driver + ", exit " + std::to_string( Result ) + "): " + ErrorMessage ) );
        return false;
    }

    return true;
}

bool Volt::Backend::Llvm::LlvmBackend::State::LinkSharedLibrary ( std::string_view ObjectPath, std::string_view OutputPath )
{
    const llvm::ErrorOr<std::string> Driver = FindLinkerDriver();
    if ( not Driver )
    {
        static_cast<void>( Fail( "llvm: no C compiler driver ('cc', 'clang', 'gcc') found on PATH to link with" ) );
        return false;
    }

    // `-shared -fPIC`: InitTarget already selected Reloc::PIC_ for every
    // build, so the object itself needs no different codegen — only the
    // driver's final link mode changes.
    std::vector<std::string> Args{ *Driver, "-shared", "-fPIC", "-o", std::string( OutputPath ), std::string( ObjectPath ) };

    for ( const std::string &Extra : Options.ExtraLinkInputs )
    {
        Args.push_back( Extra );
    }

    if ( llvm::sys::findProgramByName( "mold" ) )
    {
        Args.emplace_back( "-fuse-ld=mold" );
    }
    else if ( llvm::sys::findProgramByName( "ld.lld" ) )
    {
        Args.emplace_back( "-fuse-ld=lld" );
    }

    for ( const std::string &Library : ExternLibraries() )
    {
        if ( std::string Flag = LibraryFlag( Library ); not Flag.empty() )
        {
            Args.emplace_back( std::move( Flag ) );
        }
    }

    std::vector<llvm::StringRef> ArgRefs;
    ArgRefs.reserve( Args.size() );
    for ( const std::string &Arg : Args )
    {
        ArgRefs.emplace_back( Arg );
    }

    std::string ErrorMessage;
    bool bExecutionFailed = false;
    const int Result = llvm::sys::ExecuteAndWait( *Driver, ArgRefs, std::nullopt, {}, 0, 0, &ErrorMessage, &bExecutionFailed );
    if ( bExecutionFailed or Result != 0 )
    {
        static_cast<void>(
            Fail( "llvm: shared-object link failed (" + *Driver + ", exit " + std::to_string( Result ) + "): " + ErrorMessage ) );
        return false;
    }

    return true;
}
