#pragma once

#include "Driver_export.hpp"
#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/Core/Container/NonCopyable.hpp"
#include "Volt/Core/Container/NonMovable.hpp"
#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Diagnostics/SourceManager.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Driver/CircuitGraph.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Sema/Layout/TypeBinder.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"
#include "Volt/Sema/Link/InterfaceRegistry.hpp"
#include "Volt/Sema/Pass.hpp"

#include <cstddef>
#include <cstdint>
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
    class DRIVER_EXPORT CompileUnit : public FNonMovable, FNonCopyable
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
        // Expression types inferred for this unit alone (see SemaType.hpp).
        Sema::UnitTypes Types;
        // Callee resolutions snapshotted for this unit alone at the end of
        // TypeChecker (see Layout/CalleeMap.hpp) — the backend's read side
        // of the rules/core-ast.md operator/call protocol.
        Sema::UnitCallees Callees;
        // Lexical scopes + name bindings resolved for this unit alone
        // (see Scope/ScopeTable.hpp).
        Sema::ScopeTable Scopes;
        Sema::PassStats Stats;
    };

    struct CompileResult
    {

        std::size_t Files  = 0;
        std::size_t Errors = 0;
        // Every per-unit counter, summed. One field rather than a copy of
        // PassStats' shape, so a new counter reaches `check --metrics` with
        // no change here at all.
        Sema::PassStats Stats;
        bool bCycle = false;
    };

    // Front-end orchestrator: discovers the files of a build, parses and
    // runs the sema passes over each of them across a jthread pool, and
    // gathers every diagnostic into one thread-safe engine.
    class DRIVER_EXPORT Driver
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
        // interface publication and no analysis. `volt parse` dumps this raw
        // AST; with bLowered, the AST lowering passes (EPassKind::Lowering)
        // additionally run over each unit, so `volt parse --lowered` shows
        // the tree the analysis passes actually see.
        CompileResult ParseFiles ( const std::vector<std::string> &Paths, bool bLowered = false );

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

        // The layouts bound from the stdlib's annotations (empty before
        // compilation). Read-only for callers.
        [[nodiscard]] const Sema::TypeStore &Layouts () const
        {
            return Types;
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

        // --- The backend seam ---------------------------------------------

        [[nodiscard]] std::size_t UnitCount () const
        {
            return Units.size();
        }

        [[nodiscard]] const CompileUnit &Unit ( std::size_t Index ) const
        {
            return Units[Index];
        }

        // Every compiled unit as the read-only view a backend consumes
        // (BackendCore/BackendInput.hpp), ordered so that a dependency is
        // always emitted before its dependents — the *circuit link order* that
        // lets a single-pass emitter see every callee's declaring unit before
        // the call site's.
        //
        // The order comes from CircuitGraph::TopoOrder; a unit whose module is
        // not in the graph (a flat `CompileFiles` build has no `@[Link]` edges
        // at all) keeps its discovery order, appended after the graph's. A
        // cycle is already a hard error upstream, so a failed TopoOrder simply
        // degrades to discovery order rather than dropping units on the floor.
        [[nodiscard]] std::vector<Backend::UnitView> MakeBackendViews () const;

    private:

        struct SourceRef
        {

            std::string Path;
            std::string Module;
            bool bComponent = false;
        };

        // How far CompileRefs drives each unit past the parallel parse.
        enum class EPipeline : std::uint8_t
        {
            Full,          // publish interfaces (serial), then every sema pass
            ParseOnly,     // stop after parse (`volt parse`)
            ParseAndLower, // parse + AST lowering passes (`volt parse --lowered`)
        };

        // Register + read every SourceRef into a CompileUnit, then run the
        // pipeline over all of them up to the requested phase. Fills Units
        // (and Registry when the full sema phase runs).
        CompileResult CompileRefs ( const std::vector<SourceRef> &Refs, EPipeline Pipeline = EPipeline::Full );

        // Lex + parse one already-registered unit. Safe to call from any
        // worker thread: only `Bag` and `Unit` are touched.
        void ParseOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag );

        // Run the sema passes over one parsed unit, with read-only access to
        // the published cross-unit interfaces. Same thread-safety contract.
        void RunSemaOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag );

        // Run only the AST lowering passes over one parsed unit — no
        // cross-unit interfaces involved. Same thread-safety contract.
        void LowerOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag );

        // Read a file into the SourceManager; returns false (and reports)
        // when it cannot be read.
        [[nodiscard]] bool Load ( const std::string &Path, Core::FileId &OutFile, std::string &OutText );

        // Populate Circuit from the parsed units' top-level `@[Link]`
        // annotations and report any dependency cycle.
        void BuildLinkGraph ( const std::string &RootModule );

        // Discover and append stdlib sources from source/Lib/ if present.
        void LoadStdLib ( std::vector<SourceRef> &Refs );

        // Report a file-less, driver-level diagnostic against <driver>.
        void ReportDriver ( Core::ESeverity Severity, std::string Message );

        Core::SourceManager Sources;
        Core::DiagEngine Diagnostics;
        CircuitGraph Circuit;
        Sema::InterfaceRegistry Registry;
        // One store for the whole build, not one per unit: `Int32` is declared
        // in source/Lib/ and used everywhere, and a unit's Symbols mean
        // nothing outside it. Filled serially, then read-only.
        Sema::TypeStore Types;
        std::deque<CompileUnit> Units;
        Core::FileId DriverFile;
    };

} // namespace Driver

} // namespace Volt
