#pragma once

#include "Volt/Core/Container/NonCopyable.hpp"
#include "Volt/Core/Container/NonMovable.hpp"
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

namespace Frontend
{
    struct FAstDumpOptions;
} // namespace Frontend

namespace Driver
{

    // One source file's complete compilation state. Each unit owns its own
    // StringInterner and AstContext, so parse + sema for different files
    // never touch shared mutable state — the whole circuit compiles in
    // parallel without a lock on the hot path (only DiagEngine::Merge does).
    //
    // Non-movable on purpose: the AstContext caches `&Interner`, so a unit
    // must stay put. std::deque gives us stable addresses without moves.
    class CompileUnit : public FNonMovable, FNonCopyable
    {

    public:

        CompileUnit ( Core::FileId InFile, std::string InPath, std::string InModule, bool bInComponent )
            : File( InFile ), Path( std::move( InPath ) ), Module( std::move( InModule ) ), bComponent( bInComponent ),
              Ast( Interner, InFile )
        {
        }

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

        // Parse-only pipeline over a flat list of files: lex + parse, no
        // interface publication and no sema. `volt parse` dumps this raw AST.
        CompileResult ParseFiles ( const std::vector<std::string> &Paths );

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

        // Every gathered diagnostic (warnings included), for callers that
        // want to render even on a successful compile.
        [[nodiscard]] std::size_t DiagnosticCount () const
        {
            return Diagnostics.Count();
        }

        void RenderDiagnostics ( std::ostream &Out ) const
        {
            Diagnostics.Render( Sources, Out );
        }

        // Dump every unit's AST as the human tree (`volt parse` output).
        void DumpUnits ( std::ostream &Out, const Frontend::FAstDumpOptions &Options ) const;

    private:

        struct SourceRef
        {

            std::string Path;
            std::string Module;
            bool bComponent = false;
        };

        // Register + read every SourceRef into a CompileUnit, then run the
        // pipeline over all of them: parse (parallel), then — unless bParseOnly —
        // publish interfaces (serial) and sema (parallel). Fills Units (and
        // Registry when sema runs).
        CompileResult CompileRefs ( const std::vector<SourceRef> &Refs, bool bParseOnly = false );

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
