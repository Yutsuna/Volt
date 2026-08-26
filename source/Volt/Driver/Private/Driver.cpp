#include "Volt/Driver/Driver.hpp"

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Diagnostics/Diagnostic.hpp"
#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/Core/Support/ContentHash.hpp"
#include "Volt/Core/Support/PhaseTimer.hpp"
#include "Volt/Driver/WellKnown.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstDump.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/ConstructorSynthesis.hpp"
#include "Volt/Frontend/AST/EnumSynthesis.hpp"
#include "Volt/Frontend/AST/PointFreeLowering.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"
#include "Volt/MiddleEnd/Analysis/Raii/OwnershipInference.hpp"
#include "Volt/MiddleEnd/ConstEval/MacroEngine.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "Volt/MiddleEnd/Optimisations/InlineSummary.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{

[[nodiscard]] bool HasSuffix ( std::string_view Path, std::string_view Suffix )
{
    return Path.size() >= Suffix.size() and Path.substr( Path.size() - Suffix.size() ) == Suffix;
}

// The running `volt` binary's own path. Linux-only (`/proc/self/exe`) since
// that is the only platform this toolchain currently targets (rules/cpp-style.md).
[[nodiscard]] fs::path ExecutablePath ()
{
    std::error_code Ec;
    fs::path Exe = fs::read_symlink( "/proc/self/exe", Ec );
    if ( Ec )
    {
        return {};
    }
    return Exe;
}

// Where the running `volt` binary itself lives, so stdlib discovery does not
// depend on the caller's CWD.
[[nodiscard]] fs::path ExecutableDir ()
{
    const fs::path Exe = ExecutablePath();
    return Exe.empty() ? fs::path{} : Exe.parent_path();
}

// Resolves `source/Lib` independent of CWD, in priority order:
//   1. `VOLT_STDLIB_DIR` env override — explicit escape hatch for dev/test.
//   2. `<exe_dir>/../share/volt/Lib` — the conventional install layout, so
//      this keeps working once a real `install()` rule ships (no cost today).
//   3. The checked-out `source/Lib` baked in at configure time
//      (`VOLT_DEV_STDLIB_DIR`, see Driver/CMakeLists.txt) — guarantees a
//      dev build finds the real stdlib regardless of the invocation's CWD.
[[nodiscard]] fs::path ResolveStdlibDir ()
{
    if ( const char *Override = std::getenv( "VOLT_STDLIB_DIR" ); Override != nullptr and *Override != '\0' )
    {
        return { Override };
    }

    std::error_code Ec;
    if ( const fs::path ExeDir = ExecutableDir(); not ExeDir.empty() )
    {
        if ( const fs::path Installed = ExeDir / ".." / "share" / "volt" / "Lib"; fs::is_directory( Installed, Ec ) )
        {
            return Installed;
        }
    }

#if defined( VOLT_DEV_STDLIB_DIR )
    return { VOLT_DEV_STDLIB_DIR };
#else
    return "source/Lib";
#endif
}

[[nodiscard]] bool IsComponentPath ( std::string_view Path )
{
    return HasSuffix( Path, Volt::Driver::WellKnown::ComponentExt );
}

[[nodiscard]] bool IsSourceFile ( const fs::path &Path )
{
    const std::string Ext = Path.extension().string();
    return Ext == Volt::Driver::WellKnown::SourceExt or Ext == Volt::Driver::WellKnown::ComponentExt;
}

// Every stdlib source file under LibDir, sorted by path. Load-bearing order
// (see LoadStdLib's comment on TypeBinder) — the one walk both LoadStdLib and
// the frontend cache key must agree on, so they never see a different stdlib.
[[nodiscard]] std::vector<fs::path> CollectSortedStdlibFiles ( const fs::path &LibDir )
{
    std::vector<fs::path> Files;
    std::error_code Ec;
    if ( not fs::is_directory( LibDir, Ec ) )
    {
        return Files;
    }
    for ( const fs::directory_entry &It : fs::recursive_directory_iterator( LibDir, Ec ) )
    {
        if ( It.is_regular_file() and IsSourceFile( It.path() ) )
        {
            Files.push_back( It.path() );
        }
    }
    std::ranges::sort( Files );
    return Files;
}

// Conservative MVP fingerprint for `FrontendCacheKey`'s compiler-identity
// term: the running binary's size + mtime. Any compiler rebuild invalidates
// every stdlib cache — safe, if coarser than hashing just the AST/token/pass
// manifests (a refinement left for later, per the plan file).
[[nodiscard]] std::uint64_t CompilerBuildFingerprint ()
{
    const fs::path Exe = ExecutablePath();
    if ( Exe.empty() )
    {
        return 0;
    }

    std::error_code Ec;
    const std::uintmax_t Size = fs::file_size( Exe, Ec );
    if ( Ec )
    {
        return 0;
    }
    const fs::file_time_type MTime = fs::last_write_time( Exe, Ec );
    if ( Ec )
    {
        return 0;
    }

    std::uint64_t State = Volt::Core::FnvOffsetBasis;
    State               = Volt::Core::CombineHash( State, static_cast<std::uint64_t>( Size ) );
    State               = Volt::Core::CombineHash( State, static_cast<std::uint64_t>( MTime.time_since_epoch().count() ) );
    return State;
}

// FrontendCacheKey = Hash(CompilerBuildFingerprint | sorted(source/Lib/** path+content))
// (.agents/PROGRESS-issue-61.md, Phase 1). Nothing consults this as a cache
// yet — this phase only has to prove the key is deterministic and
// change-sensitive in isolation.
[[nodiscard]] std::uint64_t ComputeFrontendCacheKey ()
{
    const std::uint64_t Seed          = Volt::Core::CombineHash( Volt::Core::FnvOffsetBasis, CompilerBuildFingerprint() );
    const std::vector<fs::path> Files = CollectSortedStdlibFiles( ResolveStdlibDir() );
    return Volt::Core::HashFileTree( Files, Seed ).value_or( Seed );
}

// Bumping this invalidates every on-disk cache unconditionally (a
// Serialize.hpp layout change is not otherwise reflected in FrontendCacheKey,
// which only hashes stdlib source + the running binary's own identity).
// Bumped to 02 when `@[Apply]` was deleted: `Member` lost `bApply` and
// `CalleeEntry` gained `bIndirect`, so both cached records changed shape.
// Bumped to 03 when `@[ExceptionRoot]` was deleted: `TypeStore` lost its
// serialised `ExceptionRoot` field.
// Bumped to 04 when `@[Unhandled]` was deleted: `Member` lost its serialised
// `bUnhandled` field.
// Bumped to 05 when `MiddleEnd::TypeSystem::Member` gained `bReturnsOwned`: the store's cache
// is a reflected aggregate dump, so a new field silently shifts every byte
// after it. The magic is the only thing standing between a stale cache and a
// misread signature table.
// Bumped to 06 when `SynthesizeFinalizeStubs` stopped excluding generic
// types: the stdlib's own generic aggregates (`Hash<K,V>`) now carry a
// synthesized `finalize` member that a cache written by the previous
// compiler does not. Unlike the bumps above, no serialised *field* changed —
// the cache key hashes stdlib *sources*, which did not change either, so
// nothing else would have invalidated it and every build would keep reading
// a store whose member tables predate the synthesis.
// Bumped to 07 when `MiddleEnd::TypeSystem::Member` gained `ParamEscapes`: a new serialised
// field, so the same byte-shift reasoning as 05 applies.
// Bumped to 08 for the RAII epic's second half. No serialised *field* changed
// — the same "content, not shape" case as 06, and for two reasons at once:
// `SynthesizeFinalizeStubs` now resolves a type's ancestors (so a subclass no
// longer synthesizes a stub that shadows its base's destructor, and its
// triviality bit accounts for the chain), and `Raii::InferReturnOwnership`
// moved to a seam that reads the *lowered* AST, which changes what it can
// prove for every stdlib member. Both are baked into the cached store and the
// cached ASTs, and neither would invalidate the key on its own: it hashes
// stdlib *sources*, which did not change, plus the `volt` executable's own
// identity — which does not move when only a module `.so` is relinked.
// Bumped for "VOLTFE09": expression types moved out of the per-unit arena and
// into the store's canonical TypeUniverse, so the store now carries them and a
// unit carries only its ExprId -> SemaTypeId mapping (TypeUniverse.hpp). An
// older file would deserialize into a shape that no longer exists.
// Bumped to 10 when member visibility landed: `Frontend::Field`,
// `Frontend::Method` and `MiddleEnd::TypeSystem::Member` each gained an
// `EVisibility`, and all three are reflected aggregate dumps, so the same
// byte-shift reasoning as 05 and 07 applies to every cached AST *and* store.
// Bumped to 11 when we added the IO/ library
// Bumped to 12 when PassStats gained its nine inlining counters
// (MiddleEnd/Core/Pass.hpp): PassStats is a reflected aggregate dump written
// per unit by WriteFrontendCache, so the same byte-shift reasoning as 05, 07
// and 10 applies — an older file would deserialize a shorter struct into a
// longer one and silently misread every field after it.
// Bumped to 13 when `SynthesizedFunctions` joined the per-unit payload. It had
// been missing since the table existed, and stayed invisible only because no
// *concrete* stdlib member had ever contained a closure literal: a cache hit
// restored the `FuncAddr` nodes without the table naming their targets, and
// BackendLLVM's EmitFuncAddr — whose only other source is the cross-unit
// TypeStore, which a lifted closure is deliberately never in — failed the
// build. `String#blank?` (`all? { | ch | ch.whitespace? }`) is the first one.
// Bumped to 14 for issue #75's macro model. Two independent reasons, and the
// key would notice neither on its own (it hashes stdlib *sources*, which did
// not change, plus the executable's identity, which does not move when a
// module `.so` is relinked). First, shape: `Frontend::MacroDef` no longer
// carries a source-text body but a parsed `ParamList` / `TypeId` / `StmtList`,
// and `MacroBlock` is a new declaration alternative — both are reflected
// aggregate dumps inside every cached AST, so the same byte-shift reasoning as
// 05, 07 and 10 applies. Second, content: `ConstEval::ExpandTypeMacros` now
// runs at the interface seam, so a cached stdlib AST and store may contain
// methods that *no source text declares* — they were written by a macro when
// the cache was built. A cache from an older compiler has the declarations
// without the expansion, which is exactly the "content, not shape" case 06 and
// 08 already made.
// Bumped to "VOLTFE15" when `Frontend::LocalDecl` gained `bAlreadyLive`: the
// cached AST is a reflected aggregate dump, so a new field shifts every byte
// after it — the same reasoning as 05, 07, 10 and 13.
inline constexpr std::uint64_t FrontendCacheMagic = 0x564f4c54'46453135ULL; // "VOLTFE15"

// `<hex Key>/frontend.cache`, under Volt::Driver::StdlibCacheDir(Key).
[[nodiscard]] fs::path FrontendCacheFilePath ( std::uint64_t Key )
{
    const fs::path Dir = Volt::Driver::StdlibCacheDir( Key );
    return Dir.empty() ? fs::path{} : Dir / "frontend.cache";
}

} // namespace

std::filesystem::path Volt::Driver::StdlibCacheDir ( std::uint64_t FrontendKey )
{
    fs::path Base;
    if ( const char *Xdg = std::getenv( "XDG_CACHE_HOME" ); Xdg != nullptr and *Xdg != '\0' )
    {
        Base = Xdg;
    }
    else if ( const char *Home = std::getenv( "HOME" ); Home != nullptr and *Home != '\0' )
    {
        Base = fs::path( Home ) / ".cache";
    }
    else
    {
        return {};
    }
    return Base / "volt" / "stdlib" / Core::ToHex( FrontendKey );
}

Volt::Driver::Driver::Driver () : FrontendKey( ComputeFrontendCacheKey() )
{
    // A synthetic source so file-less driver diagnostics (unreadable file,
    // dependency cycle) still resolve to a valid FileId when rendered —
    // SourceManager lookups are not bounds-checked.
    DriverFile = Sources.AddFile( "<driver>", std::string{} );

    // Not logged here: FLogger's default MinLevel is Debug (prints
    // unconditionally, Logger.cpp), and no --verbose/quiet gate exists yet
    // (that lands with the CLI flags in Phase 5) — every `volt parse`/`check`
    // constructs a Driver, and Golden tests diff that stdout byte-for-byte.
}

std::uint64_t Volt::Driver::ComputeNativeCacheKey ( std::uint64_t FrontendKey,
                                                    std::string_view TargetTriple,
                                                    std::string_view OptLevel,
                                                    std::string_view ArtifactKind,
                                                    bool bLto )
{
    std::uint64_t State = Core::CombineHash( Core::FnvOffsetBasis, FrontendKey );
    State               = Core::CombineHash( State, TargetTriple );
    State               = Core::CombineHash( State, OptLevel );
    State               = Core::CombineHash( State, ArtifactKind );
    State               = Core::CombineHash( State, static_cast<std::uint64_t>( bLto ? 1 : 0 ) );
    return State;
}

std::optional<fs::path> Volt::Driver::DiscoverManifest ( const fs::path &InPath )
{
    std::error_code Ec;
    fs::path Dir = fs::absolute( fs::is_directory( InPath, Ec ) ? InPath : InPath.parent_path(), Ec );

    while ( !Dir.empty() )
    {
        fs::path Candidate = Dir / WellKnown::ManifestName;
        if ( fs::is_regular_file( Candidate, Ec ) )
        {
            return Candidate;
        }
        const fs::path Parent = Dir.parent_path();
        if ( Parent == Dir )
        {
            break;
        }
        Dir = Parent;
    }
    return std::nullopt;
}

// NOLINTNEXTLINE(performance-unnecessary-value-param)
void Volt::Driver::Driver::ReportDriver ( Core::ESeverity Severity, std::string Message )
{
    Diagnostics.Report( Core::Diagnostic{ .Severity = Severity,
                                          .Range    = Core::SourceRange{ .File = DriverFile, .Begin = 0, .End = 0 },
                                          .Message  = std::move( Message ),
                                          .Notes    = {} } );
}

bool Volt::Driver::Driver::Load ( const std::string &Path, Core::FileId &OutFile, std::string &OutText )
{
    std::ifstream Stream( Path, std::ios::binary );
    if ( not Stream )
    {
        ReportDriver( Core::ESeverity::Error, "cannot read '" + Path + "'" );
        return false;
    }

    std::ostringstream Buffer;
    Buffer << Stream.rdbuf();
    OutText = Buffer.str();
    OutFile = Sources.AddFile( Path, OutText );
    return true;
}

void Volt::Driver::Driver::ParseOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag )
{
    const std::string_view Text = Sources.TextOf( Unit.File );

    Frontend::Lexer Lexer( Unit.File, Text, Unit.Interner, Bag );
    std::vector<Frontend::Token> Tokens = Lexer.Tokenize();

    Frontend::Parser Parser( std::move( Tokens ), Unit.Ast, Bag, Text );
    if ( Unit.bComponent )
    {
        Parser.ParseComponentFile();
    }
    else
    {
        Parser.ParseFile();
    }

    // Must run before BindUnitTypes below (the cross-unit seam that builds
    // the free-function table from Ast.TopDecls): a Method a Sema pass adds
    // later would never be found by name (see PointFreeLowering.hpp).
    Frontend::LowerPointFreeDefs( Unit.Ast );

    // Same seam, same reason (see EnumSynthesis.hpp): an EnumCase's ordinal
    // must be materialized, and a payload-less enum's synthesized
    // `to_value` must exist, before TypeBinder's Phase A freezes each
    // type's member list.
    Frontend::MaterializeEnumOrdinals( Unit.Ast );
    Frontend::SynthesizeEnumMembers( Unit.Ast );
    Frontend::SynthesizeDefaultConstructors( Unit.Ast );
}

void Volt::Driver::Driver::RunSemaLoweringOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag )
{
    MiddleEnd::Core::PassContext Context{ .Ast      = Unit.Ast,
                                          .Types    = Types,
                                          .Values   = Unit.Types,
                                          .Scopes   = Unit.Scopes,
                                          .Diags    = Bag,
                                          .Stats    = Unit.Stats,
                                          .Globals  = &Registry,
                                          .Sources  = &Sources,
                                          .Callees  = &Unit.Callees,
                                          .Synth    = Unit.Synth,
                                          .AllUnits = DriverUnitAsts };
    static_cast<void>(
        MiddleEnd::Core::RunPasses( Context, std::numeric_limits<int>::min(), MiddleEnd::Core::LoweredSeamOrder() ) );
}

void Volt::Driver::Driver::RunSemaTypedOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag )
{
    MiddleEnd::Core::PassContext Context{ .Ast      = Unit.Ast,
                                          .Types    = Types,
                                          .Values   = Unit.Types,
                                          .Scopes   = Unit.Scopes,
                                          .Diags    = Bag,
                                          .Stats    = Unit.Stats,
                                          .Globals  = &Registry,
                                          .Sources  = &Sources,
                                          .Callees  = &Unit.Callees,
                                          .Synth    = Unit.Synth,
                                          .AllUnits = DriverUnitAsts };
    static_cast<void>(
        MiddleEnd::Core::RunPasses( Context, MiddleEnd::Core::LoweredSeamOrder(), std::numeric_limits<int>::max() ) );
}

void Volt::Driver::Driver::LowerOne ( CompileUnit &Unit, Core::DiagEngine::Bag &Bag )
{
    // Lowerings rewrite purely local state; no published interfaces.
    MiddleEnd::Core::PassContext Context{ .Ast     = Unit.Ast,
                                          .Types   = Types,
                                          .Values  = Unit.Types,
                                          .Scopes  = Unit.Scopes,
                                          .Diags   = Bag,
                                          .Stats   = Unit.Stats,
                                          .Sources = &Sources,
                                          .Synth   = Unit.Synth };
    static_cast<void>( MiddleEnd::Core::RunPasses( Context, MiddleEnd::Core::EPassKind::Lowering ) );
}

// The cross-unit seam, over whichever units are not already done.
//
// `bDone` is indexed by unit ordinal: a true entry is a unit whose interface,
// types, signatures and analysis flags are already in the store, so it is
// neither republished nor re-analysed and every whole-program pass receives a
// null AST for it. Two callers, one meaning — CompileRefs marks a cache-hit
// stdlib prefix, and CompileOneMore marks everything except the line it is
// compiling. Extracted rather than duplicated so those two cannot drift on
// what "already done" implies.
void Volt::Driver::Driver::RunSerialSeam ( const std::vector<bool> &bDone, std::vector<const Frontend::AstContext *> &OutAsts )
{
    Core::PhaseScope InterfaceTiming( "seam.interface" );

    Core::DiagEngine::Bag SeamBag = Core::DiagEngine::MakeBag();
    for ( std::size_t Index = 0; Index < Units.size(); ++Index )
    {
        if ( bDone[Index] )
        {
            continue; // already published and bound
        }
        const auto Ordinal = static_cast<std::uint32_t>( Index );
        static_cast<void>( MiddleEnd::Resolver::PublishUnitInterface( Units[Index].Ast, Ordinal, Registry ) );
        // Same seam, same reason: type binding is cross-unit, so it
        // cannot be a per-file parallel pass. Once this loop ends the
        // store is frozen and sema reads it without a lock.
        static_cast<void>( MiddleEnd::Resolver::BindUnitTypes( Units[Index].Ast, Ordinal, Types, SeamBag ) );
    }

    // Every unit's Phase A is done, so every type this build declares
    // exists; attach the structural layout of every non-@[Primitive] one
    // now, before any signature is resolved. Indexed the same way
    // Member::Unit / NominalType::Unit are — discovery order — so this
    // is the array ResolveStructLayouts recurses across when a field
    // names an aggregate declared in a different file. An already-done
    // unit passes null — its layouts are already attached, and
    // ResolveStructLayouts documents null entries as skipped.
    OutAsts.clear();
    OutAsts.reserve( Units.size() );
    for ( std::size_t Index = 0; Index < Units.size(); ++Index )
    {
        OutAsts.push_back( bDone[Index] ? nullptr : &Units[Index].Ast );
    }
    MiddleEnd::Resolver::ResolveStructLayouts( OutAsts, Types, &SeamBag );

    // Still serial, still the same seam, right after field layouts land
    // and before any signature is resolved: synthesize an empty
    // `finalize` for every non-generic Struct/Class that declares none
    // of its own but has a cascade-candidate field
    // (rules/raii-ownership.md — SynthesizeFinalizeStubs's
    // own doc comment has the full contract). Needs a *mutable* AST per
    // unit (it splices a new Method Decl into a type's own Body), unlike
    // ResolveStructLayouts' read-only UnitAsts above — built the same
    // way, a null entry for an already-done unit skipped identically.
    std::vector<Frontend::AstContext *> MutableUnitAsts;
    MutableUnitAsts.reserve( Units.size() );
    for ( std::size_t Index = 0; Index < Units.size(); ++Index )
    {
        MutableUnitAsts.push_back( bDone[Index] ? nullptr : &Units[Index].Ast );
    }
    MiddleEnd::Resolver::SynthesizeFinalizeStubs( MutableUnitAsts, Types );

    // Still the same serial seam, and the last thing that may *add* to the
    // store: evaluate every `macro def` for its target type, graft the
    // method it generates into that type's Body and register it as a
    // member, then run the `macro do` blocks in file order. It sits here
    // rather than in a pass because a pass is per-unit, parallel, and holds
    // a read-only store — while a macro-generated method has to become a
    // member of a type that may live in another unit entirely, before the
    // signature loop below can resolve it (MacroEngine.hpp).
    std::vector<const Frontend::AstContext *> AllUnitAsts;
    AllUnitAsts.reserve( Units.size() );
    for ( const CompileUnit &U : Units )
    {
        AllUnitAsts.push_back( &U.Ast );
    }
    MiddleEnd::ConstEval::ExpandTypeMacros( MutableUnitAsts, Types, Sources, SeamBag, AllUnitAsts );

    // Still serial, still the same seam, but a second pass: a signature
    // may name a type declared in a file that comes later, so every name
    // must exist before any signature is resolved — otherwise stdlib file
    // order would silently decide what a member returns.
    for ( std::size_t Index = 0; Index < Units.size(); ++Index )
    {
        if ( bDone[Index] )
        {
            continue;
        }
        MiddleEnd::Resolver::ResolveUnitSignatures( Units[Index].Ast, static_cast<std::uint32_t>( Index ), Types, SeamBag );
    }

    MiddleEnd::Resolver::ComputeAllVTableSlots( Types );
    Types.ComputeSubtypeIntervals();

    Diagnostics.Merge( std::move( SeamBag ) );
    InterfaceTiming.Stop();
}

// The whole-program ownership fixpoints, over the same null-AST convention.
//
// Safe to run with every unit but one nulled out, which is what makes an
// incremental line cost what the first line cost: InferReturnOwnership skips a
// member whose flag is already true, InferParameterEscape only sizes a slot
// that was never sized, and AnalyzeInlineCandidates only rewrites a verdict
// whose AST it was handed. None of the three reads a CalleeMap, which is what
// makes them safe to run after TypeStore::Functions has grown and invalidated
// every Member pointer an earlier unit resolved.
void Volt::Driver::Driver::RunOwnershipSeam ( const std::vector<const Frontend::AstContext *> &UnitAsts )
{
    const Core::PhaseScope Timing( "seam.ownership" );
    MiddleEnd::Analysis::Raii::InferReturnOwnership( UnitAsts, Types );
    // The mirror question, same seam and same reasons: whether a callee
    // keeps what it is handed. Independent of the fixpoint above — one
    // reasons about results, the other about arguments — so the order
    // between the two is free.
    MiddleEnd::Analysis::Raii::InferParameterEscape( UnitAsts, Types );
    MiddleEnd::Optimisations::AnalyzeInlineCandidates( UnitAsts, Types );
}

Volt::Driver::CompileResult
Volt::Driver::Driver::CompileRefs ( const std::vector<SourceRef> &Refs, EPipeline Pipeline, std::size_t StdlibCount )
{
    // register every file's text and unit up front so
    // the parallel phases only touch per-unit state + the diag engine.
    for ( const SourceRef &Ref : Refs )
    {
        Core::FileId File;
        std::string Text;
        if ( not Load( Ref.Path, File, Text ) )
        {
            continue;
        }
        Units.emplace_back( File, Ref.Path, Ref.Module, Ref.bComponent );
        // Every unit interns into the *build's* dictionary, so `G<A>` is one
        // handle no matter which file wrote it (TypeUniverse.hpp). Bound here,
        // before anything can type an expression — an unbound UnitTypes answers
        // no type at all, which is the loud failure this ordering avoids.
        Units.back().Types.BindUniverse( Types.Universe() );
    }

    // A file that failed to load never got a Units entry, so a caller's
    // StdlibCount (computed against Refs, not Units) could overshoot on a
    // read failure — clamp rather than let later range math underflow.
    StdlibCount = std::min( StdlibCount, Units.size() );

    // Workers pull unit indices from a shared atomic and accumulate into
    // a thread-local Bag, merged once at the end (the only lock on the
    // hot path).
    const auto ForEachUnitParallel = [&] ( auto Step, std::size_t Begin, std::size_t End )
    {
        const std::size_t Count    = End > Begin ? End - Begin : 0;
        const std::size_t Hardware = std::max<std::size_t>( 1, std::thread::hardware_concurrency() );
        const std::size_t Workers  = std::min( Hardware, std::max<std::size_t>( 1, Count ) );

        std::atomic<std::size_t> Next{ Begin };

        std::vector<std::jthread> Pool;
        Pool.reserve( Workers );
        for ( std::size_t WorkIdx = 0; WorkIdx < Workers; ++WorkIdx )
        {
            Pool.emplace_back(
                [&]
                {
                    Core::DiagEngine::Bag Bag = Core::DiagEngine::MakeBag();
                    for ( ;; )
                    {
                        const std::size_t Index = Next.fetch_add( 1, std::memory_order_relaxed );
                        if ( Index >= End )
                        {
                            break;
                        }
                        ( this->*Step )( Units[Index], Bag );
                    }
                    Diagnostics.Merge( std::move( Bag ) );
                } );
        }
    }; // jthreads join at the lambda's end

    // A cache hit fills Types/Registry/Units[0..StdlibCount) directly, so
    // ParseOne/the seam/RunSemaOne all skip that range below. StdlibCount ==
    // 0 (ParseFiles' tooling path, or the isolated warm-compile
    // WriteFrontendCache drives) never consults a cache at all. bNoCache and
    // bFresh both force a read-miss (bFresh still writes a refreshed cache
    // below; bNoCache skips that write too).
    const bool bCacheReadAllowed = not ActiveCacheOptions.bNoCache and not ActiveCacheOptions.bFresh;
    const bool bStdlibCacheHit =
        Pipeline == EPipeline::Full and StdlibCount > 0 and bCacheReadAllowed and TryLoadFrontendCache( StdlibCount );

    // parallel: lex + parse every unit into its own arenas (skipping a
    // cache-loaded stdlib prefix, which is already parsed).
    {
        const Core::PhaseScope Timing( "parse" );
        ForEachUnitParallel( &Driver::ParseOne, bStdlibCacheHit ? StdlibCount : 0, Units.size() );
    }

    DriverUnitAsts.clear();
    DriverUnitAsts.reserve( Units.size() );
    for ( std::size_t Index = 0; Index < Units.size(); ++Index )
    {
        DriverUnitAsts.push_back( &Units[Index].Ast );
    }

    if ( Pipeline == EPipeline::Full )
    {
        // A cache-hit stdlib prefix is already published, bound, signed and
        // analysed — it came out of the cache that way — so the seam skips it
        // and every whole-program pass below sees a null AST in its slot.
        std::vector<bool> bDone( Units.size(), false );
        if ( bStdlibCacheHit )
        {
            for ( std::size_t Index = 0; Index < StdlibCount; ++Index )
            {
                bDone[Index] = true;
            }
        }

        // Declared out here rather than inside the seam because the ownership
        // fixpoints below read it too.
        std::vector<const Frontend::AstContext *> UnitAsts;
        RunSerialSeam( bDone, UnitAsts );

        if ( bStdlibCacheHit )
        {
            // The seam is done for the *whole* build, so Types will not grow
            // again this run — only now is it safe to resolve every cached
            // stdlib unit's (Unit, DeclId) fixup key into a live Member*
            // (rules/ast-rewrite.md's concern in spirit: never hold/produce
            // an arena pointer across a later Add()).
            for ( std::size_t Index = 0; Index < StdlibCount; ++Index )
            {
                Units[Index].Callees.FixupDecls( Types );
            }
        }

        const std::size_t SemaBegin = bStdlibCacheHit ? StdlibCount : 0;

        // parallel, first half: desugar every unit and resolve its scopes.
        {
            const Core::PhaseScope Timing( "sema.lowering" );
            ForEachUnitParallel( &Driver::RunSemaLoweringOne, SemaBegin, Units.size() );
        }

        // serial, between the halves: whether a member hands its caller an
        // owned value is a whole-program fixpoint over every declared body, so
        // it needs every unit at once — and `TypeChecker`, which reads the
        // answer, runs in parallel immediately after, so the answer must
        // already be still by then.
        //
        // It sits *here*, rather than up in the interface seam, because it
        // reads bodies: before the lowerings it would see `Interp`, `Index`,
        // `Pipeline` and `Section` nodes instead of the ordinary calls they
        // become, and every question it asks of one would have to be answered
        // by guessing what that node will lower to — which is precisely how a
        // Volt method spelling once ended up hardcoded in this analysis
        // (rules/raii-ownership.md's own guardrail). Move the question,
        // never teach it a spelling.
        //
        // `UnitAsts` (the read-only view built above) is what it wants:
        // nothing here mutates an AST, only the store's own `Member` records.
        RunOwnershipSeam( UnitAsts );

        // parallel, second half: typing, and every check built on typing.
        // Stdlib units are typed in the first wave so their bodies are completely
        // settled and read-only before user units run BlockInliner.
        {
            const Core::PhaseScope Timing( "sema.typed" );
            if ( SemaBegin < StdlibCount )
            {
                ForEachUnitParallel( &Driver::RunSemaTypedOne, SemaBegin, StdlibCount );
            }
            ForEachUnitParallel( &Driver::RunSemaTypedOne, std::max( SemaBegin, StdlibCount ), Units.size() );
        }

        if ( not bStdlibCacheHit and StdlibCount > 0 and not Diagnostics.HasErrors() and not ActiveCacheOptions.bNoCache )
        {
            WriteFrontendCache(
                std::vector<SourceRef>( Refs.begin(), Refs.begin() + static_cast<std::ptrdiff_t>( StdlibCount ) ) );
        }
    }
    else if ( Pipeline == EPipeline::ParseAndLower )
    {
        // Lowered parse (`volt parse --lowered`): rewrite each unit's AST
        // in place, still without the cross-unit seam.
        ForEachUnitParallel( &Driver::LowerOne, 0, Units.size() );
    }

    CompileResult Result;
    Result.Files  = Units.size();
    Result.Errors = Diagnostics.ErrorTotal();
    for ( const CompileUnit &Unit : Units )
    {
        Result.Stats.Merge( Unit.Stats );
    }
    return Result;
}

void Volt::Driver::Driver::LoadStdLib ( std::vector<SourceRef> &Refs )
{
    // `recursive_directory_iterator` follows readdir() order, which is
    // filesystem-dependent, not alphabetical. TypeBinder's Phase A binds each
    // file's layout in the same pass it declares the type (TypeBinder.cpp,
    // `BindType`), so a field naming a type from a file visited later in this
    // walk (e.g. `String#data : Pointer<UInt8>` when "Pointer.vl" has not
    // been bound yet) silently resolves to an invalid LayoutId — a bug no
    // existing test caught, since TypeChecker never reads Layout and only
    // codegen (BackendLLVM) does. CollectSortedStdlibFiles fixes the order
    // (sorted by path, `Primitives/Pointer.vl` binds before
    // `Primitives/String.vl` on every filesystem) — the same walk the
    // frontend cache key hashes, so the two never disagree on "the stdlib".
    for ( const fs::path &Path : CollectSortedStdlibFiles( ResolveStdlibDir() ) )
    {
        Refs.push_back( SourceRef{ .Path = Path.string(), .Module = "Core", .bComponent = IsComponentPath( Path.string() ) } );
    }
}

bool Volt::Driver::Driver::TryLoadFrontendCache ( std::size_t StdlibCount )
{
    const fs::path CacheFile = FrontendCacheFilePath( FrontendKey );
    if ( CacheFile.empty() )
    {
        return false;
    }

    std::ifstream Stream( CacheFile, std::ios::binary );
    if ( not Stream )
    {
        return false; // no cache yet: an ordinary miss, not a warning.
    }
    std::ostringstream Buffer;
    Buffer << Stream.rdbuf();
    const std::string Bytes = Buffer.str();

    std::vector<std::byte> Data( Bytes.size() );
    std::memcpy( Data.data(), Bytes.data(), Bytes.size() );
    Meta::Reader R{ std::span<const std::byte>{ Data } };

    std::uint64_t Magic     = 0;
    std::uint64_t StoredKey = 0;
    std::uint32_t UnitCount = 0;
    bool bOk = Meta::Deserialize( R, Magic ) and Magic == FrontendCacheMagic and Meta::Deserialize( R, StoredKey ) and
               StoredKey == FrontendKey and Types.DeserializeCache( R ) and Registry.DeserializeCache( R ) and
               Meta::Deserialize( R, UnitCount ) and UnitCount == static_cast<std::uint32_t>( StdlibCount );

    for ( std::size_t Index = 0; bOk and Index < StdlibCount; ++Index )
    {
        CompileUnit &Unit = Units[Index];
        bOk               = Unit.Ast.DeserializeCache( R ) and Meta::DeserializeInterner( R, Unit.Interner ) and
              Unit.Types.DeserializeCache( R ) and Unit.Callees.DeserializeCache( R ) and Unit.Scopes.DeserializeCache( R ) and
              Unit.Synth.DeserializeCache( R ) and Meta::Deserialize( R, Unit.Stats );
    }

    if ( bOk and not R.Failed() )
    {
        return true;
    }

    // Truncated, corrupt, or key-mismatched: reset every object this attempt
    // may have touched and fall through to an ordinary fresh compile — a
    // cache is never allowed to crash or half-populate live state. Types and
    // a unit's Interner reset via Clear() rather than assignment: both
    // embed (or are) a StringInterner, whose bump allocator is neither
    // copyable nor movable.
    Types.Clear();
    Registry = MiddleEnd::Resolver::InterfaceRegistry{};
    for ( std::size_t Index = 0; Index < StdlibCount; ++Index )
    {
        CompileUnit &Unit = Units[Index];
        Unit.Interner.Clear();
        Unit.Ast   = Frontend::AstContext{ Unit.Interner, Unit.File };
        Unit.Types = MiddleEnd::TypeSystem::UnitTypes{};
        Unit.Types.BindUniverse( Types.Universe() ); // reset dropped the binding CompileRefs made
        Unit.Callees = MiddleEnd::IR::UnitCallees{};
        Unit.Scopes  = MiddleEnd::Resolver::ScopeTable{};
        Unit.Stats   = MiddleEnd::Core::PassStats{};
    }
    ReportDriver( Core::ESeverity::Warning, "stdlib frontend cache is missing, corrupt, or stale; recompiling" );
    return false;
}

void Volt::Driver::Driver::WriteFrontendCache ( const std::vector<SourceRef> &StdlibRefs ) const
{
    const fs::path CacheFile = FrontendCacheFilePath( FrontendKey );
    if ( CacheFile.empty() )
    {
        return; // neither XDG_CACHE_HOME nor HOME is set: silently skip.
    }

    // Isolated, throwaway Driver: its own Types/Units start empty and end up
    // containing exactly the stdlib's slice, so nothing here needs to slice
    // a shared arena that (in the real Driver) may already have absorbed
    // user declarations. StdlibCount == 0 on this inner call is what stops
    // it from trying to consult or write a cache itself (no recursion).
    Driver Warm;
    if ( Warm.FrontendKey != FrontendKey )
    {
        return; // paranoia: only publish a cache the warm build itself would trust.
    }
    static_cast<void>( Warm.CompileRefs( StdlibRefs, EPipeline::Full, 0 ) );
    if ( Warm.HasErrors() )
    {
        return; // never cache a broken stdlib compile.
    }

    Meta::Writer W;
    Meta::Serialize( W, FrontendCacheMagic );
    Meta::Serialize( W, FrontendKey );
    Warm.Layouts().SerializeCache( W );
    Warm.Interfaces().SerializeCache( W );

    const auto UnitCount = static_cast<std::uint32_t>( Warm.UnitCount() );
    Meta::Serialize( W, UnitCount );
    for ( std::size_t Index = 0; Index < Warm.UnitCount(); ++Index )
    {
        const CompileUnit &Unit = Warm.Unit( Index );
        Unit.Ast.SerializeCache( W );
        Meta::SerializeInterner( W, Unit.Interner );
        Unit.Types.SerializeCache( W );
        Unit.Callees.SerializeCache( W );
        Unit.Scopes.SerializeCache( W );
        Unit.Synth.SerializeCache( W );
        Meta::Serialize( W, Unit.Stats );
    }

    // Atomic publish: a per-process tmp file, then rename() over the real
    // path — a concurrent reader only ever observes a complete file.
    std::error_code Ec;
    fs::create_directories( CacheFile.parent_path(), Ec );
    const fs::path TmpFile = CacheFile.parent_path() / ( "tmp." + std::to_string( ::getpid() ) );
    {
        std::ofstream Out( TmpFile, std::ios::binary | std::ios::trunc );
        if ( not Out )
        {
            return;
        }
        Out.write( reinterpret_cast<const char *>( W.Data().data() ), static_cast<std::streamsize>( W.Data().size() ) );
    }
    fs::rename( TmpFile, CacheFile, Ec );
    if ( Ec )
    {
        std::error_code RemoveEc;
        fs::remove( TmpFile, RemoveEc );
    }
}

Volt::Driver::CompileResult Volt::Driver::Driver::CompileFiles ( const std::vector<std::string> &Paths, FCacheOptions CacheOpts )
{
    ActiveCacheOptions = CacheOpts;
    if ( CacheOpts.bNoStdlib and ( CacheOpts.bNoCache or CacheOpts.bFresh ) )
    {
        ReportDriver( Core::ESeverity::Warning, "--no-stdlib disables the stdlib entirely; ignoring "
                                                "--no-stdlib-cache/--fresh-stdlib" );
    }

    if ( CacheOpts.bVerbose )
    {
        Core::FLogger::Info( "Frontend cache key: " + Core::ToHex( FrontendKey ), "driver" );
    }

    std::vector<SourceRef> Refs;
    if ( not CacheOpts.bNoStdlib )
    {
        LoadStdLib( Refs );
    }
    StdlibUnitCountValue = Refs.size();
    Refs.reserve( Refs.size() + Paths.size() );
    for ( const std::string &Path : Paths )
    {
        Refs.push_back( SourceRef{ .Path = Path, .Module = std::string{}, .bComponent = IsComponentPath( Path ) } );
    }
    return CompileRefs( Refs, EPipeline::Full, StdlibUnitCountValue );
}

Volt::Driver::CompileResult
Volt::Driver::Driver::ParseFiles ( const std::vector<std::string> &Paths, bool bLowered, bool bResolved )
{
    if ( bResolved )
    {
        std::vector<SourceRef> Refs;
        LoadStdLib( Refs );
        StdlibUnitCountValue = Refs.size();
        Refs.reserve( Refs.size() + Paths.size() );
        for ( const std::string &Path : Paths )
        {
            Refs.push_back( SourceRef{ .Path = Path, .Module = std::string{}, .bComponent = IsComponentPath( Path ) } );
        }
        return CompileRefs( Refs, EPipeline::Full, StdlibUnitCountValue );
    }

    std::vector<SourceRef> Refs;
    Refs.reserve( Paths.size() );
    for ( const std::string &Path : Paths )
    {
        Refs.push_back( SourceRef{ .Path = Path, .Module = std::string{}, .bComponent = IsComponentPath( Path ) } );
    }
    return CompileRefs( Refs, bLowered ? EPipeline::ParseAndLower : EPipeline::ParseOnly );
}

void Volt::Driver::Driver::BuildLinkGraph ( const std::string &RootModule )
{
    Circuit.AddModule( RootModule );

    // Every top-level `@[Link("Target")]` annotation is an edge from the
    // file's owning module to Target.
    for ( const CompileUnit &Unit : Units )
    {
        const std::string &From = Unit.Module.empty() ? RootModule : Unit.Module;
        Circuit.AddModule( From );

        for ( const Frontend::DeclId Id : Unit.Ast.TopDecls )
        {
            const auto *Annotation = std::get_if<Frontend::Annotation>( &Unit.Ast.Decl( Id ) );
            if ( Annotation == nullptr or Unit.Ast.Text( Annotation->Name ) != WellKnown::LinkAnnotation )
            {
                continue;
            }
            if ( Annotation->Args.Size() == 0 )
            {
                continue;
            }
            if ( const std::optional<std::string_view> Target = Frontend::AsStringText( Unit.Ast, Annotation->Args[0] ) )
            {
                Circuit.AddLink( From, std::string{ *Target } );
            }
        }
    }

    if ( const std::vector<CircuitGraph::NodeIndex> Cycle = Circuit.FindCycle(); !Cycle.empty() )
    {
        std::string Chain;
        for ( std::size_t I = 0; I < Cycle.size(); ++I )
        {
            Chain += Circuit.NameOf( Cycle[I] );
            if ( I + 1 < Cycle.size() )
            {
                Chain += " -> ";
            }
        }
        ReportDriver( Core::ESeverity::Error, "circuit has a dependency cycle: " + Chain );
    }
}

Volt::Driver::CompileResult Volt::Driver::Driver::CompileCircuit ( const std::string &ProjectPath, FCacheOptions CacheOpts )
{
    ActiveCacheOptions = CacheOpts;
    if ( CacheOpts.bNoStdlib and ( CacheOpts.bNoCache or CacheOpts.bFresh ) )
    {
        ReportDriver( Core::ESeverity::Warning, "--no-stdlib disables the stdlib entirely; ignoring "
                                                "--no-stdlib-cache/--fresh-stdlib" );
    }

    // Phase 5: log cache keys when verbose mode is enabled
    if ( CacheOpts.bVerbose )
    {
        Core::FLogger::Info( "Frontend cache key: " + Core::ToHex( FrontendKey ), "driver" );
    }

    // The manifest is its own unit; parse it to read entrypoint + modules.
    Core::FileId ProjectFile;
    std::string ProjectText;
    if ( not Load( ProjectPath, ProjectFile, ProjectText ) )
    {
        CompileResult Failed;
        Failed.Errors = Diagnostics.ErrorTotal();
        return Failed;
    }

    Core::StringInterner ManifestInterner;
    Frontend::AstContext Manifest{ ManifestInterner, ProjectFile };
    {
        Core::DiagEngine::Bag Bag = Core::DiagEngine::MakeBag();
        Frontend::Lexer Lexer( ProjectFile, ProjectText, ManifestInterner, Bag );
        Frontend::Parser Parser( Lexer.Tokenize(), Manifest, Bag, ProjectText );
        Parser.ParseFile();
        Diagnostics.Merge( std::move( Bag ) );
    }

    const fs::path ProjectDir = fs::path( ProjectPath ).parent_path();

    std::string CircuitName{ WellKnown::DefaultCircuitName };
    std::string EntryRel;
    std::vector<std::pair<std::string, std::string>> Modules; // name -> rel dir

    // Walk the circuit manifest: `entrypoint "..."` and `modules(a=>b,...)`.
    // Each recognised key is one clause over the AstQuery helpers — adding
    // a manifest key = a WellKnown constant + one clause here.
    for ( const Frontend::DeclId TopId : Manifest.TopDecls )
    {
        const auto *Circ = std::get_if<Frontend::Circuit>( &Manifest.Decl( TopId ) );
        if ( Circ == nullptr )
        {
            continue;
        }
        CircuitName = std::string{ Manifest.Text( Circ->Name ) };

        for ( const Frontend::StmtId StmtId : Circ->Body )
        {
            const Frontend::Call *Call = Frontend::StmtAsCall( Manifest, StmtId );
            if ( Call == nullptr )
            {
                continue;
            }
            const std::optional<std::string_view> Name = Frontend::CalleeName( Manifest, *Call );

            if ( Name == WellKnown::EntrypointKey and Call->Args.Size() > 0 )
            {
                if ( const std::optional<std::string_view> Ep = Frontend::AsStringText( Manifest, Call->Args[0] ) )
                {
                    EntryRel = std::string{ *Ep };
                }
            }
            else if ( Name == WellKnown::ModulesKey )
            {
                for ( const Frontend::ExprId ArgId : Call->Args )
                {
                    const Frontend::Binary *Pair = Frontend::AsBinaryOp( Manifest, ArgId, Frontend::TokenKind::FatArrow );
                    if ( Pair == nullptr )
                    {
                        continue;
                    }
                    const std::optional<std::string_view> Key = Frontend::AsStringText( Manifest, Pair->Lhs );
                    const std::optional<std::string_view> Dir = Frontend::AsStringText( Manifest, Pair->Rhs );
                    if ( Key and Dir )
                    {
                        Modules.emplace_back( *Key, *Dir );
                    }
                }
            }
        }
    }

    // Gather sources: the stdlib first — so it occupies ordinals
    // `0..StdlibCount-1` the same way CompileFiles' Refs do, the invariant
    // the frontend cache relies on to splice a cached stdlib slice straight
    // into Driver::Units (issue #61's blind-spot-#4 requirement) — then the
    // entrypoint (owned by the root module), then every `.vl`/`.vlx` under
    // each declared module directory.
    //
    // Same seam as CompileFiles: without the stdlib nothing claims
    // IntLiteral, so every literal in a circuit typed as nothing and the
    // whole tree came out untyped. A circuit is not a different language.
    std::vector<SourceRef> Refs;
    if ( not CacheOpts.bNoStdlib )
    {
        LoadStdLib( Refs );
    }
    StdlibUnitCountValue = Refs.size();

    if ( not EntryRel.empty() )
    {
        const fs::path Entry = ProjectDir / EntryRel;
        Refs.push_back( SourceRef{ .Path = Entry.string(), .Module = CircuitName, .bComponent = IsComponentPath( EntryRel ) } );
    }

    for ( const auto &[ModName, RelDir] : Modules )
    {
        const fs::path Dir = ProjectDir / RelDir;
        std::error_code Ec;
        if ( not fs::is_directory( Dir, Ec ) )
        {
            ReportDriver( Core::ESeverity::Warning, "module '" + ModName + "' directory not found: " + Dir.string() );
            Circuit.AddModule( ModName );
            continue;
        }
        for ( const fs::directory_entry &It : fs::recursive_directory_iterator( Dir, Ec ) )
        {
            if ( It.is_regular_file() and IsSourceFile( It.path() ) )
            {
                Refs.push_back( SourceRef{
                    .Path = It.path().string(), .Module = ModName, .bComponent = IsComponentPath( It.path().string() ) } );
            }
        }
    }

    CompileResult Result = CompileRefs( Refs, EPipeline::Full, StdlibUnitCountValue );
    BuildLinkGraph( CircuitName );

    Result.Errors = Diagnostics.ErrorTotal();
    Result.bCycle = !Circuit.FindCycle().empty();
    return Result;
}

void Volt::Driver::Driver::DumpUnits ( std::ostream &Out, const Frontend::FAstDumpOptions &Options, bool bUserUnitsOnly ) const
{
    const std::size_t StartIndex = ( bUserUnitsOnly and StdlibUnitCountValue < Units.size() ) ? StdlibUnitCountValue : 0;
    for ( std::size_t Index = StartIndex; Index < Units.size(); ++Index )
    {
        const CompileUnit &Unit = Units[Index];
        Frontend::AstDumper Dumper( Unit.Ast, Sources, Out, Options );
        Dumper.DumpFile();
    }
}

// --- Incremental compilation (`volt repl`) ---------------------------------

std::size_t Volt::Driver::Driver::AppendUnit ( std::string Label, std::string Text )
{
    const Core::FileId File = Sources.AddFile( std::move( Label ), std::move( Text ) );

    // One module name for every appended unit, so a `def` in one and a call in
    // the next share a namespace rather than each getting one of their own.
    const std::size_t Index = Units.size();
    Units.emplace_back( File, std::string( Sources.PathOf( File ) ), "Main", /*bInComponent=*/false );
    Units.back().Types.BindUniverse( Types.Universe() );

    Core::DiagEngine::Bag Bag = Core::DiagEngine::MakeBag();
    ParseOne( Units[Index], Bag );
    Diagnostics.Merge( std::move( Bag ) );

    return Index;
}

Volt::Driver::Driver::UnitResult Volt::Driver::Driver::AnalyzeUnit ( const std::size_t Index )
{
    UnitResult Result;
    Result.Ordinal  = static_cast<std::uint32_t>( Index );
    Result.DiagMark = Diagnostics.Mark();

    DriverUnitAsts.clear();
    DriverUnitAsts.reserve( Units.size() );
    for ( std::size_t Slot = 0; Slot < Units.size(); ++Slot )
    {
        DriverUnitAsts.push_back( &Units[Slot].Ast );
    }

    // Everything but this unit is finished business.
    std::vector<bool> bDone( Units.size(), true );
    bDone[Index] = false;

    std::vector<const Frontend::AstContext *> UnitAsts;
    RunSerialSeam( bDone, UnitAsts );

    Core::DiagEngine::Bag LoweringBag = Core::DiagEngine::MakeBag();
    RunSemaLoweringOne( Units[Index], LoweringBag );
    Diagnostics.Merge( std::move( LoweringBag ) );

    RunOwnershipSeam( UnitAsts );

    Core::DiagEngine::Bag TypedBag = Core::DiagEngine::MakeBag();
    RunSemaTypedOne( Units[Index], TypedBag );
    Diagnostics.Merge( std::move( TypedBag ) );

    // Only this unit's diagnostics decide whether it compiled. The engine still
    // holds every earlier one's, and asking HasErrors() here would make one bad
    // unit poison everything after it.
    Result.bOk = not Diagnostics.HasErrorsSince( Result.DiagMark );
    return Result;
}

void Volt::Driver::Driver::ConsumeLineDiagnostics ( const std::size_t Mark, std::ostream &Out )
{
    Diagnostics.RenderSince( Mark, Sources, Out );
    Diagnostics.TruncateTo( Mark );
}

Volt::Backend::UnitView Volt::Driver::Driver::ViewOf ( const std::size_t Index ) const
{
    const CompileUnit &Source = Units[Index];
    return Backend::UnitView{ .Ordinal = static_cast<std::uint32_t>( Index ),
                              .Module  = Source.Module,
                              .Path    = Source.Path,
                              .Ast     = &Source.Ast,
                              .Values  = &Source.Types,
                              .Callees = &Source.Callees,
                              .Scopes  = &Source.Scopes,
                              .Synth   = &Source.Synth };
}

std::vector<Volt::Backend::UnitView> Volt::Driver::Driver::MakeBackendViews () const
{
    std::vector<Backend::UnitView> Views;
    Views.reserve( Units.size() );

    std::vector<bool> bEmitted( Units.size(), false );

    const auto Append = [&] ( std::size_t Index )
    {
        const CompileUnit &Source = Units[Index];
        // The ordinal is the *discovery* index — the very one BindUnitTypes
        // stamped on every Member and NominalType — not the position in this
        // reordered vector.
        Views.push_back( Backend::UnitView{ .Ordinal = static_cast<std::uint32_t>( Index ),
                                            .Module  = Source.Module,
                                            .Path    = Source.Path,
                                            .Ast     = &Source.Ast,
                                            .Values  = &Source.Types,
                                            .Callees = &Source.Callees,
                                            .Scopes  = &Source.Scopes,
                                            .Synth   = &Source.Synth } );
        bEmitted[Index] = true;
    };

    // Dependencies first. A module usually spans several files, so every unit
    // claiming that module name goes out together, in discovery order.
    if ( std::vector<CircuitGraph::NodeIndex> Order; Circuit.TopoOrder( Order ) )
    {
        for ( const CircuitGraph::NodeIndex Node : Order )
        {
            const std::string &Name = Circuit.NameOf( Node );
            for ( std::size_t Index = 0; Index < Units.size(); ++Index )
            {
                if ( not bEmitted[Index] and Units[Index].Module == Name )
                {
                    Append( Index );
                }
            }
        }
    }

    // Whatever the graph did not name: a flat `CompileFiles` build declares no
    // `@[Link]` edge at all, and the stdlib units are pulled in outside the
    // circuit. Discovery order already puts source/Lib/ first.
    for ( std::size_t Index = 0; Index < Units.size(); ++Index )
    {
        if ( not bEmitted[Index] )
        {
            Append( Index );
        }
    }

    return Views;
}
