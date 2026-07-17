#pragma once

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Diagnostics/SourceManager.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Driver/CircuitGraph.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"
#include "Volt/Sema/Link/InterfaceRegistry.hpp"
#include "Volt/Sema/Pass.hpp"

#include <cstddef>
#include <deque>
#include <ostream>
#include <string>
#include <vector>

namespace Volt
{

namespace Driver
{

    // One source file's complete compilation state. Each unit owns its own
    // StringInterner and AstContext, so parse + sema for different files
    // never touch shared mutable state — the whole circuit compiles in
    // parallel without a lock on the hot path (only DiagEngine::Merge does).
    //
    // Non-movable on purpose: the AstContext caches `&Interner`, so a unit
    // must stay put. std::deque gives us stable addresses without moves.
    class CompileUnit
    {

    public:

        CompileUnit ( Core::FileId InFile, std::string InPath, std::string InModule, bool bInComponent )
            : File( InFile ), Path( std::move( InPath ) ), Module( std::move( InModule ) ), bComponent( bInComponent ), Ast( Interner, InFile )
        {
        }

        CompileUnit ( const CompileUnit & )           = delete;
        CompileUnit &operator=( const CompileUnit & ) = delete;
        CompileUnit ( CompileUnit && )                = delete;
        CompileUnit &operator=( CompileUnit && )      = delete;

        ~CompileUnit () = default;

        Core::FileId File;
        std::string Path;
        std::string Module;
        bool bComponent = false;
        Core::StringInterner Interner;
        Frontend::AstContext Ast;
        Sema::TypeStore Types;
        Sema::PassStats Stats;
    };

    struct CompileResult
    {

        std::size_t Files      = 0;
        std::size_t JsxLowered = 0;
        std::size_t Errors     = 0;
        bool bCycle            = false;
    };

    // Front-end orchestrator: discovers the files of a build, parses and
    // runs the sema passes over each of them across a jthread pool, and
    // gathers every diagnostic into one thread-safe engine.
    class Driver
    {

    public:

        Driver ()
        {
            // A synthetic source so file-less driver diagnostics (unreadable
            // file, dependency cycle) still resolve to a valid FileId when
            // rendered — SourceManager lookups are not bounds-checked.
            DriverFile = Sources.AddFile( "<driver>", std::string{} );
        }

        // Compile a flat list of files (single file, or an explicit set).
        CompileResult CompileFiles ( const std::vector<std::string> &Paths );

        // Compile a whole circuit given its `Project.vl` manifest: resolve
        // the declared modules, gather their sources + the entrypoint, build
        // the `@[Link]` graph (rejecting cycles), then compile in parallel.
        CompileResult CompileCircuit ( const std::string &ProjectPath );

        [[nodiscard]] const CircuitGraph &Graph () const
        {
            return Circuit;
        }

        // The cross-unit interfaces published between the parse and sema
        // phases (empty before compilation). Read-only for callers.
        [[nodiscard]] const Sema::InterfaceRegistry &Interfaces () const
        {
            return Registry;
        }

        [[nodiscard]] bool HasErrors () const
        {
            return Diagnostics.HasErrors();
        }

        void RenderDiagnostics ( std::ostream &Out ) const
        {
            Diagnostics.Render( Sources, Out );
        }

        // Print the parsed + lowered AST of every compiled unit (golden-test
        // / debug hook). Units keep their AST after compilation.
        void PrintUnits ( std::ostream &Out ) const;

    private:

        struct SourceRef
        {

            std::string Path;
            std::string Module;
            bool bComponent = false;
        };

        // Register + read every SourceRef into a CompileUnit, then run the
        // pipeline over all of them: parse (parallel), publish interfaces
        // (serial), sema (parallel). Fills Units and Registry.
        CompileResult CompileRefs ( const std::vector<SourceRef> &Refs );

        // Lex + parse one already-registered unit. Safe to call from any
        // worker thread: only `Bag` and `Unit` are touched.
        void ParseOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag );

        // Run the sema passes over one parsed unit, with read-only access to
        // the published cross-unit interfaces. Same thread-safety contract.
        void RunSemaOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag );

        // Read a file into the SourceManager; returns false (and reports)
        // when it cannot be read.
        [[nodiscard]] bool Load ( const std::string &Path, Core::FileId &OutFile, std::string &OutText );

        // Populate Circuit from the parsed units' top-level `@[Link]`
        // annotations and report any dependency cycle.
        void BuildLinkGraph ( const std::string &RootModule );

        // Report a file-less, driver-level diagnostic against <driver>.
        void ReportDriver ( Core::ESeverity Severity, std::string Message );

        Core::SourceManager Sources;
        Core::DiagEngine Diagnostics;
        CircuitGraph Circuit;
        Sema::InterfaceRegistry Registry;
        std::deque<CompileUnit> Units;
        Core::FileId DriverFile;
    };

} // namespace Driver

} // namespace Volt
