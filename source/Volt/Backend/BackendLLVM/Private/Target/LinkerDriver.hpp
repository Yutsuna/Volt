#pragma once

// LinkerDriver.hpp — driving the system linker over the emitted object.
//
// The link goes through a C compiler driver (`cc`), not a raw invocation of
// `ld`: only the driver knows this host's CRT startup objects, dynamic linker
// path and default libc, and reinventing that table here would be exactly the
// kind of guess rules/zero-hardcode.md's spirit warns against — never bake in
// what belongs to the platform toolchain.

#include "Target/AotServices.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class LinkerDriver
        {

        public:

            explicit LinkerDriver ( AotServices &InServices ) : Services( &InServices )
            {
            }

            // Every distinct @[External] library the build names, in first-seen
            // order — `libc`, `libm`, ... — read off the TypeStore's own members
            // rather than re-scanned from any AST, the same way SymbolOf reads a
            // single member's ExternSymbol.
            [[nodiscard]] std::vector<std::string> ExternLibraries () const;

            // Drive the system linker (mold, then LLD, then whatever `cc` picks)
            // to turn ObjectPath into a linked executable at OutputPath.
            [[nodiscard]] bool LinkExecutable ( std::string_view ObjectPath, std::string_view OutputPath );

            // Same driver, `-shared -fPIC` instead of a CRT-started executable —
            // the native-artifact cache's `--stdlib-artifact=shared` build mode.
            // No entry point is involved either way: EmitEntryPoint already
            // no-ops on an empty EntrySymbol, which this build mode sets.
            [[nodiscard]] bool LinkSharedLibrary ( std::string_view ObjectPath, std::string_view OutputPath );

        private:

            AotServices *Services = nullptr;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
