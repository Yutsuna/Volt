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
#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "Volt/MiddleEnd/Resolver/InterfaceRegistry.hpp"
#include "Volt/MiddleEnd/Resolver/TypeBinder.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Frontend
{
    struct FAstDumpOptions;
} // namespace Frontend

namespace Driver
{

    /// Walk up from InPath (file or directory) looking for a Project.vl manifest.
    [[nodiscard]] DRIVER_EXPORT std::optional<std::filesystem::path> DiscoverManifest ( const std::filesystem::path &InPath );

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
        MiddleEnd::TypeSystem::UnitTypes Types;
        // Callee resolutions snapshotted for this unit alone at the end of
        // TypeChecker (see Layout/CalleeMap.hpp) — the backend's read side
        // of the rules/core-ast.md operator/call protocol.
        MiddleEnd::IR::UnitCallees Callees;
        // Lexical scopes + name bindings resolved for this unit alone
        // (see Scope/ScopeTable.hpp).
        MiddleEnd::Resolver::ScopeTable Scopes;
        MiddleEnd::Core::PassStats Stats;
        // Functions a lowering pass synthesized for this unit alone — see
        // Sema/Layout/SynthesizedFunctions.hpp for why this is not just
        // another entry in Types (the cross-unit TypeStore).
        MiddleEnd::IR::SynthesizedFunctions Synth;
    };

    struct CompileResult
    {

        std::size_t Files  = 0;
        std::size_t Errors = 0;
        // Every per-unit counter, summed. One field rather than a copy of
        // PassStats' shape, so a new counter reaches `check --metrics` with
        // no change here at all.
        MiddleEnd::Core::PassStats Stats;
        bool bCycle = false;
    };

    // Shared knobs over the stdlib frontend cache, common to every command
    // that compiles (`build`, `check`, and — the moment they exist —
    // `run`/`repl`): see Volt::CLI::StdlibCacheOptions(), the one place the
    // CLI flags feeding this struct are declared, per cli-surface.md's
    // meta-first shape (one option-group function, reused by reference).
    struct FCacheOptions
    {

        // Bypass frontend + native caches entirely: neither consulted nor
        // written. What CI uses to get the pre-cache baseline.
        bool bNoCache = false;
        // Force a miss on read (recompute), but still publish the refreshed
        // result afterward — repairs/refreshes a cache without turning
        // caching off.
        bool bFresh = false;
        // Skip the stdlib entirely (bootstrapping/testing source/Lib/**
        // itself with no circular dependency). Implies there is nothing to
        // cache either way; bNoCache/bFresh together with this are ignored
        // with a warning, not an error.
        bool bNoStdlib = false;

        // Phase 5: verbose output for cache keys
        bool bVerbose = false;
    };

    // What `volt build` (or, once it exists, any other command emitting
    // native code) asks Driver::Build() for. Plain data — no Backend* type
    // appears here, so a caller (Volt::CLI::FBuildCommand) never includes a
    // backend header at all; Driver is the one place --target gets resolved
    // to a concrete IBackend.
    struct BuildOptions
    {

        // Backend selection (cli-surface.md): "native" -> BackendLLVM today;
        // any other value is an error Driver::Build() reports itself.
        std::string Target = "native";
        // Empty means "derive from the module name" (mirrors
        // Backend::Llvm::EmitOptions::OutputPath).
        std::string OutputPath;
        // -O2 by default: `buildO0DefaultPipeline` is only the minimal
        // semantically-required set (mem2reg + AlwaysInliner), so an -O0
        // default shipped every artifact with no inlining, no constant
        // propagation and no vectorisation at all. `-O 0` stays one flag
        // away for a debug build. Already folded into NativeCacheKey
        // below, so the stdlib artifact invalidates on its own.
        std::uint8_t OptLevel = 2;
        bool bLto             = false;
        // "", "ir", or "obj" — stop after that intermediate artifact; empty
        // runs the whole way to a linked artifact (mirrors
        // Backend::Llvm::EEmitStage).
        std::string Emit;
        std::string EntrySymbol = "main";

        // --- Issue #61: the native stdlib artifact cache (Phase 3/4) ------
        // Bypass/refresh, same semantics as FCacheOptions but for the
        // *native* artifact rather than the frontend one — kept separate
        // because it invalidates on different things (NativeCacheKey vs
        // FrontendCacheKey) and only applies when Emit is empty (a full
        // link): an intermediate `ir`/`obj` artifact never reaches the
        // linker these apply to.
        bool bStdlibArtifactNoCache = false;
        bool bStdlibArtifactFresh   = false;
        // "static" (default, an `ar`-built archive) or "shared" (`-fPIC
        // -shared`) — which native/<NativeCacheKey>.{a,so} Driver::Build()
        // builds/consumes.
        std::string StdlibArtifactKind = "static";

        // --- Issue #61 Phase 5: verbose output ---
        bool bVerbose = false;
    };

    // What one Driver::Build() produced.
    struct BuildResult
    {

        bool bOk = false;
        std::string Artifact;
        // Set only when !bOk — Driver::Build() never throws, it reports.
        std::string Message;
    };

    // What `volt run` asks for. Deliberately not a BuildOptions: half of that
    // struct is about naming an output file, and there is no output file.
    struct RunOptions
    {

        std::string Target = "native";

        // Passed through to the program as argv. argv[0] is supplied by the
        // caller, like any other exec.
        std::vector<std::string> ProgramArgs;

        // Shared objects to resolve against before the process's own symbols.
        std::vector<std::string> Dylibs;

        std::uint8_t OptLevel = 0;
        bool bVerbose         = false;
    };

    // What one Driver::Run() produced.
    struct RunResult
    {

        bool bOk = false;

        // The program's exit code. Meaningful only when bOk.
        std::int32_t Code = 0;

        std::string Message;
    };

    // Front-end orchestrator: discovers the files of a build, parses and
    // runs the sema passes over each of them across a jthread pool, and
    // gathers every diagnostic into one thread-safe engine.
    class DRIVER_EXPORT Driver
    {

    public:

        Driver ();

        // Compile a flat list of files (single file, or an explicit set).
        CompileResult CompileFiles ( const std::vector<std::string> &Paths, FCacheOptions CacheOpts = {} );

        // Parse-only pipeline over a flat list of files:
        // - default: lex + parse, no interface publication and no analysis (`volt parse`)
        // - bLowered: additionally runs AST lowering passes (`volt parse --lowered`)
        // - bResolved: full semantic pipeline, types, inlining, and lifetimes (`volt parse --resolved`)
        CompileResult ParseFiles ( const std::vector<std::string> &Paths, bool bLowered = false, bool bResolved = false );

        // Compile a whole circuit given its `Project.vl` manifest: resolve
        // the declared modules, gather their sources + the entrypoint, build
        // the `@[Link]` graph (rejecting cycles), then compile in parallel.
        CompileResult CompileCircuit ( const std::string &ProjectPath, FCacheOptions CacheOpts = {} );

        // Emit + link (or `--emit ir`/`--emit obj` stop early) an already-
        // compiled build (CompileFiles/CompileCircuit must have run first and
        // left HasErrors() false) through whichever concrete backend
        // Options.Target selects. This is the *only* place in the compiler
        // that resolves --target to a concrete IBackend: a caller (Volt::CLI
        // ::FBuildCommand) never includes a Backend* header at all, matching
        // cli-surface.md's "build -> the full Driver pipeline, then a code
        // generator selected by --target through the IBackend seam".
        //
        // Also drives issue #61's native stdlib artifact cache (Phase 3/4)
        // when Options.Emit is empty (a full link): reuses/builds
        // native/<NativeCacheKey>.{a,so} and links it in rather than
        // re-emitting the stdlib's own bodies. Never a hard failure on its
        // own — a disabled/unavailable/failed native cache silently falls
        // back to defining the stdlib in this build, exactly as if the
        // cache did not exist.
        //
        // Implemented in DriverBuild.cpp, guarded by VOLT_ENABLE_LLVM
        // internally: a build with no LLVM toolchain still links (this
        // header never mentions LLVM), it just has BuildResult::bOk == false
        // with an explanatory Message for every Target.
        [[nodiscard]] BuildResult Build ( const BuildOptions &Options );

        // Run an already-compiled build in this process, through BackendJIT
        // instead of writing an artifact — `volt run`. Same three-phase
        // protocol as Build(), same IR from the same emission layer; only the
        // tail differs, which is the whole reason BackendLlvmIr exists.
        //
        // The exit code the program returned is RunResult::Code, and it is
        // meaningful only when bOk: a false bOk means the program never got to
        // run, and Message says why.
        //
        // Implemented in DriverRun.cpp, guarded by VOLT_ENABLE_JIT internally,
        // so this header mentions no backend type — exactly like Build().
        [[nodiscard]] RunResult Run ( const RunOptions &Options );

        [[nodiscard]] const CircuitGraph &Graph () const
        {
            return Circuit;
        }

        // Hash(CompilerBuildFingerprint | sorted(source/Lib/** path+content)),
        // computed once at construction. Not yet consulted as a cache key
        // (.agents/PROGRESS-issue-61.md, Phase 1) — exposed so callers (e.g.
        // a future native-artifact cache) can fold it into their own key.
        [[nodiscard]] std::uint64_t FrontendCacheKey () const
        {
            return FrontendKey;
        }

        // Length of Units()' stdlib prefix — ordinals `0..N-1` — for a
        // native-artifact cache (or any other consumer) to slice against.
        // 0 before compilation, or when the last compile ran with
        // FCacheOptions::bNoStdlib.
        [[nodiscard]] std::size_t StdlibUnitCount () const
        {
            return StdlibUnitCountValue;
        }

        // The cross-unit interfaces published between the parse and sema
        // phases (empty before compilation). Read-only for callers.
        [[nodiscard]] const MiddleEnd::Resolver::InterfaceRegistry &Interfaces () const
        {
            return Registry;
        }

        // The layouts bound from the stdlib's annotations (empty before
        // compilation). Read-only for callers.
        [[nodiscard]] const MiddleEnd::TypeSystem::TypeStore &Layouts () const
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
        void DumpUnits ( std::ostream &Out, const Frontend::FAstDumpOptions &Options, bool bUserUnitsOnly = false ) const;

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

        // Mutable counterpart to Layouts(): a backend monomorphises generics
        // into this store's layout arena as call sites discover them
        // (BackendCore/BackendInput.hpp explains why `BackendInput::Types` is
        // a non-const pointer). Exists only for the backend seam.
        [[nodiscard]] MiddleEnd::TypeSystem::TypeStore &MutableLayouts ()
        {
            return Types;
        }

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
        //
        // StdlibCount marks the length of Refs' *stdlib prefix* (LoadStdLib
        // always runs before any user path is appended, in both CompileFiles
        // and CompileCircuit, precisely so stdlib units occupy ordinals
        // `0..StdlibCount-1` — issue #61's blind-spot-#4 requirement). 0
        // means "nothing here is cacheable" (ParseFiles' tooling path never
        // includes the stdlib at all, and the frontend-cache warm-compile
        // reuses this same function with StdlibCount == 0 so it never tries
        // to consult or write its own cache — see WriteFrontendCache).
        CompileResult
        CompileRefs ( const std::vector<SourceRef> &Refs, EPipeline Pipeline = EPipeline::Full, std::size_t StdlibCount = 0 );

        // Lex + parse one already-registered unit. Safe to call from any
        // worker thread: only `Bag` and `Unit` are touched.
        void ParseOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag );

        // Run the sema passes over one parsed unit, with read-only access to
        // the published cross-unit interfaces. Same thread-safety contract.
        //
        // Split in two at `MiddleEnd::Core::LoweredSeamOrder()`, because one serial,
        // whole-program question sits between the halves: whether a member
        // hands its caller an owned value, and whether it keeps what it is
        // handed (`Sema::Raii`). Asking it before the desugarings run forces
        // the answer to be guessed from *sugar* — an `Interp` node instead of
        // the `String#+` fold it becomes — and guessing is what puts Volt
        // method spellings into the middle-end. Asking it after them costs one
        // extra parallel dispatch and nothing else.
        void RunSemaLoweringOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag );

        // The second half: typing and every check built on typing.
        void RunSemaTypedOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag );

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

        // --- Frontend cache (Issue #61, Phase 2c) -------------------------
        //
        // Attempts to load `frontend.cache` for this Driver's FrontendKey
        // straight into Types/Registry/Units[0..StdlibCount). On any failure
        // (missing file, wrong key, truncated/corrupt stream) every touched
        // object is reset to pristine and this returns false — the caller
        // falls through to an ordinary fresh compile of the stdlib range,
        // exactly as if no cache existed. Never a crash, only ever a miss.
        [[nodiscard]] bool TryLoadFrontendCache ( std::size_t StdlibCount );

        // After a cache-miss stdlib compile succeeds (no errors), regenerates
        // the cache via an isolated, throwaway Driver compiling *only*
        // StdlibRefs (StdlibCount == 0, so that inner compile never itself
        // consults or writes a cache — no recursion). Isolating it in a
        // fresh Driver means its TypeStore/Units start empty and end up
        // containing exactly the stdlib's slice, with no need to slice a
        // shared arena that has already absorbed user declarations. Costs a
        // second stdlib compile, once, only on a cold cache — negligible
        // next to the cost this whole feature exists to amortise away.
        void WriteFrontendCache ( const std::vector<SourceRef> &StdlibRefs ) const;

        Core::SourceManager Sources;
        Core::DiagEngine Diagnostics;
        CircuitGraph Circuit;
        MiddleEnd::Resolver::InterfaceRegistry Registry;
        // One store for the whole build, not one per unit: `Int32` is declared
        // in source/Lib/ and used everywhere, and a unit's Symbols mean
        // nothing outside it. Filled serially, then read-only.
        MiddleEnd::TypeSystem::TypeStore Types;
        std::deque<CompileUnit> Units;
        std::vector<const Frontend::AstContext *> DriverUnitAsts;
        Core::FileId DriverFile;
        std::uint64_t FrontendKey = 0;
        // Set by CompileFiles/CompileCircuit before CompileRefs runs — the
        // length of Refs' stdlib prefix, exposed read-only via
        // StdlibUnitCount().
        std::size_t StdlibUnitCountValue = 0;
        // Set by CompileFiles/CompileCircuit before CompileRefs runs; read by
        // TryLoadFrontendCache/WriteFrontendCache to decide whether the cache
        // is consulted/refreshed/bypassed this run.
        FCacheOptions ActiveCacheOptions;
    };

    // NativeCacheKey = FrontendCacheKey | TargetTriple | OptLevel | ArtifactKind | LTO
    // (.agents/PROGRESS-issue-61.md, Phase 1). Free function, not a Driver
    // method: the inputs (target triple, opt level, artifact kind, LTO) are
    // backend/CLI concerns the Driver itself doesn't know — a caller with
    // that context (BuildCommand today; Phase 3/4's archive/`.so` cache
    // build later) combines them with a Driver's FrontendCacheKey() here.
    // Nothing consults this as a cache yet.
    [[nodiscard]] DRIVER_EXPORT std::uint64_t ComputeNativeCacheKey ( std::uint64_t FrontendKey,
                                                                      std::string_view TargetTriple,
                                                                      std::string_view OptLevel,
                                                                      std::string_view ArtifactKind,
                                                                      bool bLto );

    // `$XDG_CACHE_HOME/volt/stdlib/<hex FrontendKey>` (fallback
    // `~/.cache/volt/stdlib/<hex FrontendKey>`) — the one directory the
    // frontend cache (`frontend.cache`) and the native-artifact cache
    // (`native/<hex NativeCacheKey>.{a,so,meta}`) both live under, so a
    // caller building the latter (Phase 3/4's archive/.so build mode) never
    // re-derives the base path Driver's own frontend-cache code already
    // resolved. Empty when neither XDG_CACHE_HOME nor HOME is set — callers
    // treat that as "no cache available", never an error.
    [[nodiscard]] DRIVER_EXPORT std::filesystem::path StdlibCacheDir ( std::uint64_t FrontendKey );

} // namespace Driver

} // namespace Volt
