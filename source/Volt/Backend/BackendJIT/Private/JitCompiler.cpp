// JitCompiler.cpp — see JitCompiler.hpp.
//
// Every ORC call that can fail returns llvm::Error or llvm::Expected. None of
// them are allowed past this file: an Error that is neither consumed nor
// returned aborts the process on destruction, so each one is turned into a
// message here, at the point where there is still context to name what failed.

#include "JitCompiler.hpp"

#include "Volt/BackendLlvmIr/OptimizationLevel.hpp"
#include "Volt/Core/Support/PhaseTimer.hpp"

#include <llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/IRPartitionLayer.h>
#include <llvm/ExecutionEngine/Orc/IRTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITLinkRedirectableSymbolManager.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/LazyReexports.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ReOptimizeLayer.h>
#include <llvm/ExecutionEngine/Orc/RedirectionManager.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/MCTargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/TargetParser/Triple.h>

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

// Same reason ModuleContext guards it: the registry is process-global and not
// re-entrant, and the JIT may be the first thing in the process to touch it.
void InitialiseNativeTarget ()
{
    static std::once_flag Once;
    std::call_once( Once,
                    [] ()
                    {
                        llvm::InitializeNativeTarget();
                        llvm::InitializeNativeTargetAsmPrinter();
                        llvm::InitializeNativeTargetAsmParser();
                        // The REPL's `:asm` reads back the bytes ORC mapped;
                        // without this the target registry has a printer and
                        // no decoder, and lookupTarget succeeds only to fail
                        // one call later.
                        ( void )llvm::InitializeNativeTargetDisassembler();
                    } );
}

// llvm::toString consumes the Error, which is exactly what every call site here
// wants: the message survives, the Error does not.
std::string Consume ( llvm::Error Err )
{
    return llvm::toString( std::move( Err ) );
}

// Every module that reaches ORC, appended to the file VOLT_JIT_DUMP_IR names.
// The JIT has no `--emit ir` of its own — its modules are handed straight to
// ORC and never written anywhere — so without this the only way to see what
// the JIT actually compiled is a debugger.
void DumpIfAsked ( const llvm::Module &Mod )
{
    const char *Where = std::getenv( "VOLT_JIT_DUMP_IR" );
    if ( Where == nullptr )
    {
        return;
    }

    std::error_code Ec;
    llvm::raw_fd_ostream Out( std::string( Where ), Ec, llvm::sys::fs::OF_Append );
    if ( not Ec )
    {
        Mod.print( Out, nullptr );
    }
}

// Keep every definition the module already promised.
//
// ORC reads a module's symbol table when it *adds* the module and promises
// exactly that set; a transform layer runs afterwards, and materialisation
// fails outright — "Missing definitions in module ..." — if a promised symbol
// is not defined by the time the object comes out. So an optimisation pipeline
// running here is not free to delete what a pipeline running over a translation
// unit would happily delete.
//
// One linkage is discardable and reachable from outside at the same time, and
// it is the one Volt uses for mergeable bodies: linkonce(_odr). The O0
// pipeline's AlwaysInliner inlines such a body at every call site inside the
// module and then drops the out-of-line copy, having left no reference to it —
// correct for a translation unit, fatal here. Raising it to weak is exactly
// what IrOptions::bRetainMergeableBodies does at emission, for exactly this
// reason, one layer earlier.
//
// Internal and private definitions are deliberately left alone: ORC never
// promised those, so deleting one is the pipeline doing its job.
void PinPromisedSymbols ( llvm::Module &Mod )
{
    const auto Pin = [] ( llvm::GlobalValue &Value )
    {
        if ( Value.isDeclaration() or not Value.hasLinkOnceLinkage() )
        {
            return;
        }
        Value.setLinkage( Value.hasLinkOnceODRLinkage() ? llvm::GlobalValue::WeakODRLinkage : llvm::GlobalValue::WeakAnyLinkage );
    };

    for ( llvm::Function &Fn : Mod )
    {
        Pin( Fn );
    }
    for ( llvm::GlobalVariable &Global : Mod.globals() )
    {
        Pin( Global );
    }
}

// The name a re-optimised module carries its level under.
//
// A module flag rather than a second transform function, because the answer has
// to travel *with* the module: ReOptimizeLayer hands a promoted partition back
// down to the very layer the first version went through, and nothing else about
// it tells the two apart.
constexpr const char *TierFlag = "volt.jit.tier";

// PassBuilder's default pipeline for `Level`, over one module.
//
// The O0 pipeline is not "no pipeline": it is the minimal semantically-required
// set, principally mem2reg, and this emitter never builds SSA itself. The
// ahead-of-time tail says the same thing from the other side
// (BackendLLVM/Private/Target/Optimizer.cpp).
void RunPipeline ( llvm::Module &Mod, const llvm::OptimizationLevel Level )
{
    Volt::Backend::Ir::RunOptimizationPipeline( Mod, Level );
}

// What level this module asked for, if it asked at all. Only a promoted
// partition ever does.
std::optional<llvm::OptimizationLevel> TierOf ( const llvm::Module &Mod )
{
    const auto *Asked = llvm::mdconst::extract_or_null<llvm::ConstantInt>( Mod.getModuleFlag( TierFlag ) );
    if ( Asked == nullptr )
    {
        return std::nullopt;
    }
    return Volt::Backend::Ir::OptimizationLevelOf( static_cast<std::uint8_t>( Asked->getZExtValue() ) );
}

// Where a lazy call-through lands when the body it was supposed to compile
// could not be compiled.
//
// The call-through manager takes this address at construction, and the one
// thing it must not be is the default: a zero there turns a compile failure
// into a jump to a null pointer — a SIGSEGV with the trampoline's frame on the
// stack and nothing to read. It is reached *from JIT-ed code*, in the middle of the
// user's program, with that call's arguments still in registers, so there is
// nothing sensible to return: the only honest thing left is to say what
// happened and stop.
//
// bVerify is what keeps this unreachable in practice — a malformed module is
// refused at emission, where there is a diagnostic to give.
[[noreturn]] void LazyCompileFailed ()
{
    llvm::errs() << "jit: a function could not be compiled when it was first called; the program cannot continue\n";
    std::abort();
}

// What one lazy compile compiles: the function that was asked for, plus every
// function this module defines that its code *names*, transitively.
//
// LLVM ships two policies and neither is right here. `compileWholeModule` is
// eager with extra steps. `compileRequested` — one function per partition — is
// the obvious reading of "lazy", and it is what made laziness a bet rather than
// a win: each partition clones the module skeleton, so compiling n functions one
// at a time costs O(n^2)-ish, and a program that ends up calling everything it
// defines measured *68% slower* than eager.
//
// Taking the callees closes that gap without giving up the thing worth having.
// A module nothing reaches is still never compiled, and a function nothing
// calls is still never compiled — that is where all the winning happens, and it
// is untouched. What changes is only the shape of the work once a module *is*
// reached: one partition holding a call tree instead of n partitions holding a
// function each.
//
// Reached from an *instruction*, which is the line this draws. A function named
// by code that runs is one the caller is about to use: the callee of a direct
// call, and equally the body half of a closure pair, whose address is taken by
// a store in the very block that goes on to call it. A function named only by a
// *global initialiser* is not — that is a vtable, and which of its entries a
// `dyn Trait` will reach is exactly what nobody knows until it is reached. Those
// keep their stubs and stay lazy, which is what makes dynamic dispatch pay only
// for the methods it actually dispatches to.
void CollectReferenced ( const llvm::Value *From, std::vector<const llvm::Function *> &Out )
{
    if ( const auto *Fn = llvm::dyn_cast<llvm::Function>( From ); Fn != nullptr )
    {
        Out.push_back( Fn );
        return;
    }

    // A function pointer folded into a constant — an aggregate built inline for
    // a closure pair, an expression around it — is still named by this
    // instruction, so it is still evidence.
    if ( const auto *Expr = llvm::dyn_cast<llvm::Constant>( From ); Expr != nullptr )
    {
        for ( const llvm::Use &Operand : Expr->operands() )
        {
            CollectReferenced( Operand.get(), Out );
        }
    }
}

// -Wnull-dereference is on for the whole project on purpose, and this is the
// one place it is wrong. At -O3 GCC inlines llvm::Instruction's intrusive-list
// iterator (`ilist_iterator_w_bits::operator++`, ADT/ilist_node_base.h) far
// enough to see the list sentinel's null `Next` and reports walking a basic
// block as a null dereference. The sentinel is never dereferenced — it is what
// `end()` compares against — and no spelling of the loop avoids it, block-by-
// block included: the iterator itself is what GCC cannot follow.
//
// Scoped to this function rather than added to meson/meson.build's one global
// exception (-Wno-maybe-uninitialized), because the warning is worth having
// everywhere else.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"

// What one lazy compile compiles, gathered transitively.
std::optional<llvm::orc::IRPartitionLayer::GlobalValueSet>
PartitionWithCallees ( llvm::orc::IRPartitionLayer::GlobalValueSet Requested )
{
    llvm::orc::IRPartitionLayer::GlobalValueSet Partition = std::move( Requested );

    std::vector<const llvm::Function *> Pending;
    for ( const llvm::GlobalValue *Value : Partition )
    {
        if ( const auto *Fn = llvm::dyn_cast<llvm::Function>( Value ); Fn != nullptr and not Fn->isDeclaration() )
        {
            Pending.push_back( Fn );
        }
    }

    std::vector<const llvm::Function *> Named;
    while ( not Pending.empty() )
    {
        const llvm::Function *Fn = Pending.back();
        Pending.pop_back();

        for ( const llvm::Instruction &Inst : llvm::instructions( *Fn ) )
        {
            Named.clear();
            for ( const llvm::Use &Operand : Inst.operands() )
            {
                CollectReferenced( Operand.get(), Named );
            }

            for ( const llvm::Function *Referenced : Named )
            {
                // A declaration resolves through the dylib like any other
                // undefined symbol; there is no body here to put in a partition.
                if ( Referenced->isDeclaration() )
                {
                    continue;
                }
                if ( Partition.insert( Referenced ).second )
                {
                    Pending.push_back( Referenced );
                }
            }
        }
    }
    return Partition;
}

#pragma GCC diagnostic pop

// Call count threshold before a candidate function is promoted to -O2.
// 100 avoids prematurely recompiling functions during brief startup loops while
// swiftly promoting real hot paths (e.g. 300k iterations).
constexpr std::uint64_t TierThreshold = 100;

// Custom profiler for ReOptimizeLayer. Instruments only candidate functions.
// If a partition holds only trivial/straight-line functions, it is left un-instrumented
// and compiles at base O0 without any profiling penalty.
llvm::Error ProfileIfCandidate ( llvm::orc::ReOptimizeLayer &,
                                 llvm::orc::ReOptimizeLayer::ReOptMaterializationUnitID MUID,
                                 unsigned CurVersion,
                                 llvm::orc::ThreadSafeModule &Tsm )
{
    return Tsm.withModuleDo(
        [&] ( llvm::Module &Mod ) -> llvm::Error
        {
            std::vector<llvm::Function *> Candidates;
            for ( llvm::Function &Fn : Mod )
            {
                if ( Volt::Backend::Ir::IsCandidateForOptimization( Fn ) )
                {
                    Candidates.push_back( &Fn );
                }
            }

            if ( Candidates.empty() )
            {
                return llvm::Error::success();
            }

            llvm::Type *I64Ty = llvm::Type::getInt64Ty( Mod.getContext() );
            auto *Counter     = new llvm::GlobalVariable( Mod, I64Ty, false, llvm::GlobalValue::InternalLinkage,
                                                          llvm::Constant::getNullValue( I64Ty ), "__orc_reopt_counter" );

            llvm::Value *Threshold = llvm::ConstantInt::get( I64Ty, TierThreshold, true );

            for ( llvm::Function *Fn : Candidates )
            {
                llvm::BasicBlock &Entry = Fn->getEntryBlock();
                llvm::Instruction *IP   = &*Entry.getFirstInsertionPt();
                llvm::IRBuilder<> IRB( IP );

                llvm::Value *Cnt   = IRB.CreateLoad( I64Ty, Counter );
                llvm::Value *Cmp   = IRB.CreateICmpEQ( Cnt, Threshold );
                llvm::Value *Added = IRB.CreateAdd( Cnt, llvm::ConstantInt::get( I64Ty, 1 ) );
                IRB.CreateStore( Added, Counter );

                llvm::Instruction *SplitTerminator = llvm::SplitBlockAndInsertIfThen( Cmp, IP, false );
                llvm::orc::ReOptimizeLayer::createReoptimizeCall( Mod, *SplitTerminator, MUID, CurVersion );
            }
            return llvm::Error::success();
        } );
}

// A module holding only declarations resolves nothing and materialises
// nothing, so adding it to a dylib costs a symbol-table scan and buys an entry
// nobody can ever look up.
bool DefinesAnything ( const llvm::Module &Mod )
{
    for ( const llvm::Function &Fn : Mod )
    {
        if ( not Fn.isDeclaration() )
        {
            return true;
        }
    }
    for ( const llvm::GlobalVariable &Var : Mod.globals() )
    {
        if ( Var.hasInitializer() )
        {
            return true;
        }
    }
    return false;
}

} // namespace

struct Volt::Backend::Jit::JitCompiler::Impl
{

    std::unique_ptr<llvm::orc::LLJIT> Jit;

    // Everything below is declared *after* Jit so that it is destroyed
    // *before* it, and the order among them is dependency order read
    // backwards. That is not a preference: ~LLJIT ends the ExecutionSession,
    // and a ResourceTracker still held by one of these afterwards would run its
    // destructor against a session that no longer exists — which does not
    // crash, it hangs, on a mutex in freed memory.
    //
    // It is also the order LLVM itself uses. LLLazyJIT holds the same lazy
    // stack as members of a class deriving from LLJIT, so its own members go
    // first and the base's session-ending destructor goes last.

    // ReOptimizeLayer's MangleAndInterner keeps a *reference* to the DataLayout
    // it is handed, so it cannot be handed LLJIT's: that one dies last.
    llvm::DataLayout Layout;

    // Jump stubs a symbol can be repointed through, which is what makes a
    // second version of a function reachable by callers compiled against the
    // first. JITLink's implementation, because LLJIT links with JITLink.
    std::unique_ptr<llvm::orc::RedirectableSymbolManager> Redirects;

    // The three objects LLLazyJIT would have owned, plus the one it has no room
    // for. The three are null together: either this session has a lazy stack or
    // it does not.
    std::unique_ptr<llvm::orc::LazyCallThroughManager> CallThroughs;
    std::unique_ptr<llvm::orc::ReOptimizeLayer> ReOpt;
    std::unique_ptr<llvm::orc::IRPartitionLayer> Partitions;
    std::unique_ptr<llvm::orc::CompileOnDemandLayer> OnDemand;

    ECompilePolicy CompilePolicy = ECompilePolicy::Eager;

    // Whether ReOpt is in the stack. Not `ReOpt != nullptr` at the call sites
    // that ask, because what they mean is "will hot code be built twice", and
    // that is a policy answer rather than a pointer.
    bool bTiered = false;

    // One batch, added together and removed together.
    struct Generation
    {

        // Null for a generation living in the main dylib — the common case,
        // and what OpenGeneration produces. Non-null only for a replacement,
        // which needs a symbol table of its own.
        llvm::orc::JITDylib *Dylib = nullptr;

        // Null for a lazy batch, which has no tracker to be given: the modules
        // go to the dylib's default tracker (JitCompiler.hpp, DropGeneration).
        llvm::orc::ResourceTrackerSP Tracker;

        bool bLazy = false;
    };

    std::map<GenerationId, Generation> Generations;
    GenerationId NextGeneration = 1;

    // The one context every module of this session was typed in. Held here
    // rather than moved into the first ThreadSafeModule because a later
    // generation — a reload, a REPL line — has to be opened in the same one.
    llvm::orc::ThreadSafeContext Ctx;
    bool bContextAdopted = false;

    // The text of the last batch of modules added, kept only while a consumer
    // has asked for it. A REPL's `:ir` is that consumer, and it is the only
    // one — a `volt run` would render every module it compiles for nothing.
    bool bRecordIr = false;
    std::string LastIrText;
};

Volt::Backend::Jit::JitCompiler::JitCompiler () : P( std::make_unique<Impl>() )
{
}

Volt::Backend::Jit::JitCompiler::~JitCompiler () = default;

bool Volt::Backend::Jit::JitCompiler::Init ( const SessionOptions &Wanted, std::string &OutError )
{
    InitialiseNativeTarget();

    // The TargetMachine's own CodeGenOptLevel is deliberately left alone, and
    // that is worth stating because it looks like an omission.
    //
    // There are two levels, not one: the pass pipeline decides what the IR
    // looks like, CodeGenOptLevel decides how hard instruction selection and
    // register allocation then work on it. Volt's `-O` has never meant the
    // second one — the AOT tail calls createTargetMachine with no level at all
    // (BackendLlvmIr/Private/Core/ModuleContext.cpp), so `volt build -O0` gets
    // LLVM's Default there too, and LLJIT's default is the same.
    //
    // Setting it here was measured and reverted: it made `volt run -O0` mean
    // *nothing* optimised, at 364 ms on a benchmark the untouched build ran in
    // 167 ms. `-O` is one promise across both tails, so if the back-end level
    // is ever to follow it, both tails have to move together — this is not the
    // file that gets to decide it alone.
    llvm::orc::LLJITBuilder Builder;
    Builder.setNumCompileThreads( Wanted.CompileThreads );

    llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>> Built = Builder.create();
    if ( not Built )
    {
        OutError = "jit: could not create an LLJIT: " + Consume( Built.takeError() );
        return false;
    }

    P->Jit    = std::move( *Built );
    P->Layout = P->Jit->getDataLayout();

    // One LLJIT either way. The lazy stack is four objects added on top of it
    // rather than a different JIT class, which is what lets the tiering layer
    // sit in the middle of them.
    const bool bLazy = Wanted.Policy == ECompilePolicy::Lazy and BuildLazyStack( Wanted.OptLevel );
    P->CompilePolicy = bLazy ? ECompilePolicy::Lazy : ECompilePolicy::Eager;

    // What the base pipeline runs at, which is not always what `-O` said.
    //
    // A tiered session compiles *everything* at O0 and promotes what turns out
    // hot, so `-O2` names the level a second compile will reach rather than the
    // level of the first. Everything else — every eager session, and a lazy one
    // that could not build the tiering layer — takes `-O` at face value, which
    // is what 8a made it mean.
    InstallPipeline( Ir::OptimizationLevelOf( P->bTiered ? std::uint8_t{ 0 } : Wanted.OptLevel ) );
    return true;
}

bool Volt::Backend::Jit::JitCompiler::BuildLazyStack ( const std::uint8_t Tier )
{
    llvm::orc::ExecutionSession &Session = P->Jit->getExecutionSession();
    const llvm::Triple &Machine          = P->Jit->getTargetTriple();

    // Lazy needs trampolines LLVM only has for some architectures, and a
    // machine without them still deserves to run the program. The Error is
    // consumed rather than reported: falling back is not a failure, and
    // Policy() is how a caller learns it happened.
    llvm::Expected<std::unique_ptr<llvm::orc::LazyCallThroughManager>> CallThroughs =
        llvm::orc::createLocalLazyCallThroughManager( Machine, Session, llvm::orc::ExecutorAddr::fromPtr( &LazyCompileFailed ) );
    if ( not CallThroughs )
    {
        llvm::consumeError( CallThroughs.takeError() );
        return false;
    }

    // getIRTransformLayer, not the init-helper layer above it, because that one
    // is protected and LLJIT hands out no way to reach it. What it would have
    // added is the platform's module transform, which for the generic LLVM IR
    // platform rewrites llvm.global_ctors and llvm.global_dtors into init
    // functions — and Volt emits neither: initialisation is `_V_init_all`, a
    // function the entry point calls like any other.
    llvm::orc::IRLayer &Below = Tier > 0 and InstallTiering( Tier )
                                    ? static_cast<llvm::orc::IRLayer &>( *P->ReOpt )
                                    : static_cast<llvm::orc::IRLayer &>( P->Jit->getIRTransformLayer() );

    P->CallThroughs = std::move( *CallThroughs );
    P->Partitions   = std::make_unique<llvm::orc::IRPartitionLayer>( Session, Below );
    P->Partitions->setPartitionFunction( PartitionWithCallees );
    P->OnDemand = std::make_unique<llvm::orc::CompileOnDemandLayer>(
        Session, *P->Partitions, *P->CallThroughs, llvm::orc::createLocalIndirectStubsManagerBuilder( Machine ) );
    return true;
}

bool Volt::Backend::Jit::JitCompiler::InstallTiering ( const std::uint8_t Tier )
{
    // Below the partitioning, and that placement is the whole reason this
    // works at all.
    //
    // ReOptimizeLayer replaces every symbol it takes over with a jump stub it
    // can later repoint, and a stub is only a thing a *function* can have — so
    // it declines, silently and by design, any module whose interface holds a
    // data symbol. A Volt module always does: a unit with a top-level variable
    // exports it, and the type tables are external constants. Placed above the
    // partitioning it would therefore take over the stdlib units and skip the
    // one the user's hot loop is in, which is worse than not tiering at all.
    //
    // A partition is the other shape. IRPartitionLayer promotes the module's
    // internal symbols and hands down a submodule holding one call tree, whose
    // interface is the functions in it and nothing else — every global it
    // touches is a declaration resolved back to the module it was split from.
    // So every partition is eligible, and the unit of promotion becomes the
    // call tree rather than the unit, which is the granularity worth having.
    auto *Objects                       = llvm::dyn_cast<llvm::orc::ObjectLinkingLayer>( &P->Jit->getObjLinkingLayer() );
    llvm::orc::JITDylib *const Platform = P->Jit->getPlatformJITDylib().get();
    if ( Objects == nullptr or Platform == nullptr )
    {
        return false;
    }

    llvm::Expected<std::unique_ptr<llvm::orc::RedirectableSymbolManager>> Redirects =
        llvm::orc::JITLinkRedirectableSymbolManager::Create( *Objects );
    if ( not Redirects )
    {
        llvm::consumeError( Redirects.takeError() );
        return false;
    }

    auto ReOpt = std::make_unique<llvm::orc::ReOptimizeLayer>( P->Jit->getExecutionSession(), P->Layout,
                                                               P->Jit->getIRTransformLayer(), **Redirects );

    // The dispatch a promoted function calls out through. "Lite" because it is
    // the in-process stand-in for the ORC runtime, which this JIT does not load
    // — it defines __orc_rt_jit_dispatch as an absolute symbol pointing back
    // into this process rather than into a runtime library.
    if ( llvm::Error Err = ReOpt->addOrcRTLiteSupport( *Platform, P->Layout ) )
    {
        llvm::consumeError( std::move( Err ) );
        return false;
    }
    if ( llvm::Error Err = ReOpt->registerRuntimeFunctions( *Platform ) )
    {
        llvm::consumeError( std::move( Err ) );
        return false;
    }

    // All this has to do is say what level the second build wants; the
    // pipeline that reads it is the one InstallPipeline already put in the
    // layer below (TierOf).
    //
    // The module it is handed is the partition as it was *before* profiling —
    // ReOptimizeLayer keeps a pristine clone for exactly this — so the counter
    // and the dispatch call are not in the code that replaces them.
    ReOpt->setReoptimizeFunc(
        [Tier] ( llvm::orc::ReOptimizeLayer &, llvm::orc::ReOptimizeLayer::ReOptMaterializationUnitID, unsigned,
                 llvm::orc::ResourceTrackerSP, llvm::orc::ThreadSafeModule &Tsm ) -> llvm::Error
        {
            Tsm.withModuleDo( [Tier] ( llvm::Module &Mod ) { Mod.setModuleFlag( llvm::Module::Override, TierFlag, Tier ); } );
            return llvm::Error::success();
        } );

    ReOpt->setAddProfilerFunc( ProfileIfCandidate );

    P->Redirects = std::move( *Redirects );
    P->ReOpt     = std::move( ReOpt );
    P->bTiered   = true;
    return true;
}

void Volt::Backend::Jit::JitCompiler::InstallPipeline ( const llvm::OptimizationLevel Level )
{
    // Runs at O0 too, and that is the whole reason this exists rather than
    // being skipped when nobody asked for optimisation.
    //
    // LLJIT's IR transform layer is the identity by default, so until now the
    // JIT handed instruction selection exactly what the emitter wrote — and
    // what the emitter writes is an alloca per local, in the entry block, with
    // no SSA anywhere (BackendLLVM/Private/Target/Optimizer.cpp says the same
    // thing from the other tail). mem2reg lives in PassBuilder's O0 pipeline,
    // so the ahead-of-time build has always had it and the JIT never did.
    //
    // The whole-program prologue that tail runs first — InternalizePass, then
    // GlobalDCE — is deliberately absent here, and it is not an oversight: a
    // JIT module is a *fragment*. Under PerUnit every other unit's code is in
    // another module, and under Lazy a partition is a fragment of that, so
    // "nothing outside this module reaches this symbol" is false for almost
    // everything in it. Internalising on that basis would delete bodies the
    // next module is about to call.
    P->Jit->getIRTransformLayer().setTransform(
        [Level] ( llvm::orc::ThreadSafeModule Tsm,
                  const llvm::orc::MaterializationResponsibility & ) -> llvm::Expected<llvm::orc::ThreadSafeModule>
        {
            // withModuleDo takes the context lock, which is the contract
            // IRTransformLayer states: with compile threads, two modules of one
            // build are optimised at once and llvm::Type is context-owned.
            Tsm.withModuleDo(
                [Level] ( llvm::Module &Mod )
                {
                    PinPromisedSymbols( Mod );

                    // Timed, and timed under two names, because under Lazy this
                    // is where the compiling happens: it runs inside the
                    // program's own execution, long after backend.jit.add and
                    // backend.jit.materialize have closed on ~0.2 ms of
                    // bookkeeping. A promoted module is separated out because
                    // the whole question tiering asks is what the second build
                    // costs against what the first one saved.
                    //
                    // The pass pipeline only, not the instruction selection
                    // below it — a transform has no way to bracket the layer it
                    // hands the module on to. At -O2 the pipeline is the larger
                    // half, and at O0 neither half is much.
                    const std::optional<llvm::OptimizationLevel> Promoted = TierOf( Mod );
                    const Volt::Core::PhaseScope Timing( Promoted.has_value() ? "backend.jit.reoptimize"
                                                                              : "backend.jit.optimize" );
                    RunPipeline( Mod, Promoted.value_or( Level ) );
                } );
            return Tsm;
        } );
}

Volt::Backend::Jit::ECompilePolicy Volt::Backend::Jit::JitCompiler::Policy () const
{
    return P->CompilePolicy;
}

bool Volt::Backend::Jit::JitCompiler::Tiering () const
{
    return P->bTiered;
}

std::string Volt::Backend::Jit::JitCompiler::TargetTriple () const
{
    return P->Jit == nullptr ? std::string{} : P->Jit->getTargetTriple().str();
}

std::string Volt::Backend::Jit::JitCompiler::DataLayoutString () const
{
    return P->Jit == nullptr ? std::string{} : P->Jit->getDataLayout().getStringRepresentation();
}

Volt::Backend::Jit::GenerationId Volt::Backend::Jit::JitCompiler::OpenGeneration ()
{
    const GenerationId Id = P->NextGeneration++;

    Impl::Generation &Gen = P->Generations[Id];
    Gen.bLazy             = P->CompilePolicy == ECompilePolicy::Lazy;

    // A lazy batch is handed to a dylib, not to a tracker, so making one here
    // would be an object that only ever answers a question nobody may ask.
    if ( not Gen.bLazy )
    {
        Gen.Tracker = P->Jit->getMainJITDylib().createResourceTracker();
    }
    return Id;
}

bool Volt::Backend::Jit::JitCompiler::OpenReplacement ( GenerationId &OutGen, std::string &OutError )
{
    const GenerationId Id = P->NextGeneration++;

    llvm::Expected<llvm::orc::JITDylib &> Made = P->Jit->createJITDylib( "volt.gen." + std::to_string( Id ) );
    if ( not Made )
    {
        OutError = "jit: could not open a generation dylib: " + Consume( Made.takeError() );
        return false;
    }

    // Itself first, then the main dylib: what the replacement defines is what
    // the replacement's own calls should reach, and everything else — the
    // stdlib, the other units, the process — resolves exactly as before.
    Made->addToLinkOrder( P->Jit->getMainJITDylib() );
    // Also add the replacement dylib to the main dylib's link order so any
    // fresh globals declared on a line that redefines a function are visible
    // to subsequent lines in the session.
    P->Jit->getMainJITDylib().addToLinkOrder( *Made );

    // Eager unconditionally — the header says why. Note that this is a
    // property of the generation and not of the session: a lazy `volt run`
    // that later reloads gets a lazy boot generation and eager replacements,
    // which is the intended mix rather than an inconsistency.
    Impl::Generation &Gen = P->Generations[Id];
    Gen.Dylib             = &*Made;
    Gen.Tracker           = Made->createResourceTracker();
    Gen.bLazy             = false;

    OutGen = Id;
    return true;
}

bool Volt::Backend::Jit::JitCompiler::AddModules ( GenerationId Gen, Ir::OwnedModules Modules, std::string &OutError )
{
    const auto Found = P->Generations.find( Gen );
    if ( Found == P->Generations.end() )
    {
        OutError = "jit: no such generation";
        return false;
    }
    if ( Modules.Modules.empty() )
    {
        OutError = "jit: the emission produced no module";
        return false;
    }

    // One batch is one unit's emission, so the record holds the *last* unit
    // rather than everything since recording was turned on. A caller that
    // wants a unit kept out of it — the REPL's own echo line, which is not a
    // line anybody typed — turns recording off around it, and this leaves what
    // is already held alone.
    if ( P->bRecordIr )
    {
        P->LastIrText.clear();
    }

    // The context is adopted once and kept: every module of this session shares
    // it, which is what lets one module call a function another one defines. A
    // later batch — a reload, a REPL line — arrives without one and is opened
    // in the context already held.
    if ( Modules.Context != nullptr )
    {
        P->Ctx = llvm::orc::ThreadSafeContext( std::move( Modules.Context ) );
    }
    else if ( not P->bContextAdopted )
    {
        OutError = "jit: the first batch of modules brought no context";
        return false;
    }
    P->bContextAdopted = true;

    for ( std::unique_ptr<llvm::Module> &Mod : Modules.Modules )
    {
        if ( Mod == nullptr or not DefinesAnything( *Mod ) )
        {
            continue;
        }

        const std::string Name( Mod->getName() );
        DumpIfAsked( *Mod );
        if ( P->bRecordIr )
        {
            llvm::raw_string_ostream Text( P->LastIrText );
            Mod->print( Text, nullptr );
        }

        // What LLJIT::addIRModule does on the way past, and the lazy path no
        // longer goes past it. A module typed for a machine other than the one
        // that will run it is a miscompile rather than a link error, so this is
        // checked rather than assumed — the emitter asks DataLayoutString() for
        // exactly this string, which makes it a check that never fires.
        if ( Mod->getDataLayout().isDefault() )
        {
            Mod->setDataLayout( P->Layout );
        }
        else if ( Mod->getDataLayout() != P->Layout )
        {
            OutError = "jit: the module '" + Name + "' was typed for a different machine than the one running it";
            return false;
        }

        llvm::orc::ThreadSafeModule Safe( std::move( Mod ), P->Ctx );

        // The one line the whole policy comes down to. Lazy goes to the dylib
        // rather than to a tracker: the on-demand layer splits the module
        // across an implementation dylib of its own, so removing the tracker
        // this generation holds would leave half of it behind — which is
        // exactly what makes a lazy generation undroppable.
        llvm::Error Err = Found->second.bLazy ? P->OnDemand->add( Found->second.Dylib == nullptr ? P->Jit->getMainJITDylib()
                                                                                                 : *Found->second.Dylib,
                                                                  std::move( Safe ) )
                                              : P->Jit->addIRModule( Found->second.Tracker, std::move( Safe ) );
        if ( Err )
        {
            OutError = "jit: could not add the module '" + Name + "': " + Consume( std::move( Err ) );
            return false;
        }
    }
    return true;
}

bool Volt::Backend::Jit::JitCompiler::DropGeneration ( GenerationId Gen, std::string &OutError )
{
    const auto Found = P->Generations.find( Gen );
    if ( Found == P->Generations.end() )
    {
        OutError = "jit: no such generation";
        return false;
    }
    if ( Found->second.Tracker == nullptr )
    {
        OutError = "jit: a lazily compiled generation cannot be removed";
        return false;
    }
    if ( llvm::Error Err = Found->second.Tracker->remove() )
    {
        OutError = "jit: could not remove a generation: " + Consume( std::move( Err ) );
        return false;
    }
    P->Generations.erase( Found );
    return true;
}

bool Volt::Backend::Jit::JitCompiler::LookupIn ( GenerationId Gen,
                                                 std::string_view Symbol,
                                                 std::uintptr_t &OutAddr,
                                                 std::string &OutError )
{
    const auto Found = P->Generations.find( Gen );
    if ( Found == P->Generations.end() or Found->second.Dylib == nullptr )
    {
        return Lookup( Symbol, OutAddr, OutError );
    }

    llvm::Expected<llvm::orc::ExecutorAddr> Addr = P->Jit->lookup( *Found->second.Dylib, Symbol );
    if ( not Addr )
    {
        OutError = "jit: '" + std::string( Symbol ) + "' did not resolve: " + Consume( Addr.takeError() );
        return false;
    }
    OutAddr = static_cast<std::uintptr_t>( Addr->getValue() );
    return true;
}

bool Volt::Backend::Jit::JitCompiler::Lookup ( std::string_view Symbol, std::uintptr_t &OutAddr, std::string &OutError )
{
    llvm::Expected<llvm::orc::ExecutorAddr> Found = P->Jit->lookup( Symbol );
    if ( not Found )
    {
        OutError = "jit: symbol '" + std::string( Symbol ) + "' did not resolve: " + Consume( Found.takeError() );
        return false;
    }
    OutAddr = static_cast<std::uintptr_t>( Found->getValue() );
    return true;
}

bool Volt::Backend::Jit::JitCompiler::AddDylib ( std::string_view Path, std::string &OutError )
{
    const char Prefix = P->Jit->getDataLayout().getGlobalPrefix();

    llvm::Expected<std::unique_ptr<llvm::orc::DynamicLibrarySearchGenerator>> Gen =
        llvm::orc::DynamicLibrarySearchGenerator::Load( std::string( Path ).c_str(), Prefix );
    if ( not Gen )
    {
        OutError = "jit: could not load '" + std::string( Path ) + "': " + Consume( Gen.takeError() );
        return false;
    }

    P->Jit->getMainJITDylib().addGenerator( std::move( *Gen ) );
    return true;
}

bool Volt::Backend::Jit::JitCompiler::AddProcessSymbols ( std::string &OutError )
{
    const char Prefix = P->Jit->getDataLayout().getGlobalPrefix();

    llvm::Expected<std::unique_ptr<llvm::orc::DynamicLibrarySearchGenerator>> Gen =
        llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess( Prefix );
    if ( not Gen )
    {
        OutError = "jit: could not open the process's own symbols: " + Consume( Gen.takeError() );
        return false;
    }

    P->Jit->getMainJITDylib().addGenerator( std::move( *Gen ) );
    return true;
}

std::size_t Volt::Backend::Jit::JitCompiler::LiveGenerations () const
{
    return P->Generations.size();
}

void Volt::Backend::Jit::JitCompiler::RecordIr ( const bool bEnable )
{
    // Deliberately does not clear: turning recording off is how a caller
    // *excludes* a unit from the record, and clearing would throw away the
    // very thing it was protecting.
    P->bRecordIr = bEnable;
}

std::string Volt::Backend::Jit::JitCompiler::LastIr () const
{
    return P->LastIrText;
}

std::string Volt::Backend::Jit::JitCompiler::Disassemble ( const std::uintptr_t Address,
                                                           const std::size_t MaxBytes,
                                                           std::string &OutError ) const
{
    if ( P->Jit == nullptr or Address == 0 or MaxBytes == 0 )
    {
        OutError = "jit: nothing to disassemble";
        return {};
    }

    InitialiseNativeTarget();

    const llvm::Triple TheTriple = P->Jit->getTargetTriple();

    std::string Why;
    const llvm::Target *Machine = llvm::TargetRegistry::lookupTarget( TheTriple, Why );
    if ( Machine == nullptr )
    {
        OutError = "jit: no disassembler for '" + TheTriple.str() + "': " + Why;
        return {};
    }

    // Every one of these owns a piece of the target description, and the
    // context below borrows three of them — so they are declared here, in
    // destruction order, and outlive it.
    const std::unique_ptr<llvm::MCRegisterInfo> Registers( Machine->createMCRegInfo( TheTriple ) );
    if ( Registers == nullptr )
    {
        OutError = "jit: the target has no register description";
        return {};
    }

    const llvm::MCTargetOptions Options;
    const std::unique_ptr<llvm::MCAsmInfo> AsmInfo( Machine->createMCAsmInfo( *Registers, TheTriple, Options ) );
    const std::unique_ptr<llvm::MCSubtargetInfo> Subtarget( Machine->createMCSubtargetInfo( TheTriple, "", "" ) );
    const std::unique_ptr<llvm::MCInstrInfo> Instructions( Machine->createMCInstrInfo() );
    if ( AsmInfo == nullptr or Subtarget == nullptr or Instructions == nullptr )
    {
        OutError = "jit: the target description is incomplete";
        return {};
    }

    llvm::MCContext Context( TheTriple, AsmInfo.get(), Registers.get(), Subtarget.get() );
    const std::unique_ptr<llvm::MCDisassembler> Decoder( Machine->createMCDisassembler( *Subtarget, Context ) );
    const std::unique_ptr<llvm::MCInstPrinter> Printer(
        Machine->createMCInstPrinter( TheTriple, AsmInfo->getAssemblerDialect(), *AsmInfo, *Instructions, *Registers ) );
    if ( Decoder == nullptr or Printer == nullptr )
    {
        OutError = "jit: this build of LLVM has no decoder for '" + TheTriple.str() + "'";
        return {};
    }

    // The bytes ORC mapped, read as bytes. There is no other way to see them:
    // JIT-linked code is never written to a file, so what a debugger would
    // read from an object is only available here, in memory, at this address.
    const llvm::ArrayRef<std::uint8_t> Code(
        reinterpret_cast<const std::uint8_t *>( Address ), // NOLINT(performance-no-int-to-ptr)
        MaxBytes );

    std::string Out;
    llvm::raw_string_ostream Text( Out );

    std::uint64_t Offset = 0;
    while ( Offset < MaxBytes )
    {
        llvm::MCInst Instruction;
        std::uint64_t Size = 0;

        const llvm::MCDisassembler::DecodeStatus Status =
            Decoder->getInstruction( Instruction, Size, Code.slice( Offset ), Address + Offset, llvm::nulls() );
        if ( Status != llvm::MCDisassembler::Success or Size == 0 )
        {
            break;
        }

        Text << llvm::format_hex( Address + Offset, 18 ) << "  ";
        Printer->printInst( &Instruction, Address + Offset, "", *Subtarget, Text );
        Text << '\n';

        Offset += Size;

        // A function ends at its return, and reading past one would decode
        // whatever the linker happened to place next as if it belonged here.
        // Padding between functions is not always a valid instruction, so the
        // decode failure above catches the rest.
        if ( Instructions->get( Instruction.getOpcode() ).isReturn() )
        {
            break;
        }
    }

    if ( Out.empty() )
    {
        OutError = "jit: nothing decoded at that address";
    }
    return Out;
}
