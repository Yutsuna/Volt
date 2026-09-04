// OrcJitQueue.cpp — LLVM ORC implementation of IJitQueue.
//
// Manages LLJIT, LLLazyJIT, IRPartitionLayer, ReOptimizeLayer, IrGenerator,
// and the PassBuilder pipeline for Volt's JIT.

#include "OrcJitQueue.hpp"

#include "Volt/BackendCore/InitAllSynthesizer.hpp"
#include "Volt/BackendCore/UnwindTransport.hpp"
#include "Volt/BackendLlvmIr/IrGenerator.hpp"
#include "Volt/BackendLlvmIr/LlvmAccess.hpp"
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

#include <algorithm>
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

void InitialiseNativeTarget ()
{
    static std::once_flag Once;
    std::call_once( Once,
                    [] ()
                    {
                        llvm::InitializeNativeTarget();
                        llvm::InitializeNativeTargetAsmPrinter();
                        llvm::InitializeNativeTargetAsmParser();
                        ( void )llvm::InitializeNativeTargetDisassembler();
                    } );
}

std::string Consume ( llvm::Error Err )
{
    return llvm::toString( std::move( Err ) );
}

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

constexpr const char *TierFlag = "volt.jit.tier";

void RunPipeline ( llvm::Module &Mod, const llvm::OptimizationLevel Level )
{
    Volt::Backend::Ir::RunOptimizationPipeline( Mod, Level );
}

std::optional<llvm::OptimizationLevel> TierOf ( const llvm::Module &Mod )
{
    const auto *Asked = llvm::mdconst::extract_or_null<llvm::ConstantInt>( Mod.getModuleFlag( TierFlag ) );
    if ( Asked == nullptr )
    {
        return std::nullopt;
    }
    return Volt::Backend::Ir::OptimizationLevelOf( static_cast<std::uint8_t>( Asked->getZExtValue() ) );
}

[[noreturn]] void LazyCompileFailed ()
{
    llvm::errs() << "jit: a function could not be compiled when it was first called; the program cannot continue\n";
    std::abort();
}

void CollectReferenced ( const llvm::Value *From, std::vector<const llvm::Function *> &Out )
{
    if ( const auto *Fn = llvm::dyn_cast<llvm::Function>( From ); Fn != nullptr )
    {
        Out.push_back( Fn );
        return;
    }

    if ( const auto *Expr = llvm::dyn_cast<llvm::Constant>( From ); Expr != nullptr )
    {
        for ( const llvm::Use &Operand : Expr->operands() )
        {
            CollectReferenced( Operand.get(), Out );
        }
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"

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

constexpr std::uint64_t TierThreshold = 100;

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

void HarvestMeta ( const Volt::Backend::Ir::IrGenerator &Generator, Volt::Backend::Jit::CompiledUnitMeta &OutMeta )
{
    for ( const auto &Sym : Generator.LastUnitSymbols() )
    {
        OutMeta.Symbols.push_back( { .Name = Sym.Name, .Signature = Sym.Signature } );
    }
    for ( const auto &Shape : Generator.LastUnitShapes() )
    {
        OutMeta.Shapes.push_back( { .Name = Shape.Name, .Size = Shape.Size, .Alignment = Shape.Alignment } );
    }
    for ( const auto &VTab : Generator.VTableEntries() )
    {
        OutMeta.VTables.push_back( { .VTable = VTab.VTable, .Slot = VTab.Slot, .Function = VTab.Function } );
    }
    for ( const auto &Def : Generator.DefinedSymbols() )
    {
        OutMeta.DefinedSymbols.push_back( Def );
    }
}

} // namespace

struct Volt::Backend::Jit::OrcJitQueue::Impl
{

    std::unique_ptr<llvm::orc::LLJIT> Jit;
    llvm::DataLayout Layout{ "" };

    std::unique_ptr<llvm::orc::RedirectableSymbolManager> Redirects;
    std::unique_ptr<llvm::orc::LazyCallThroughManager> CallThroughs;
    std::unique_ptr<llvm::orc::ReOptimizeLayer> ReOpt;
    std::unique_ptr<llvm::orc::IRPartitionLayer> Partitions;
    std::unique_ptr<llvm::orc::CompileOnDemandLayer> OnDemand;

    ECompilePolicy CompilePolicy = ECompilePolicy::Eager;
    bool bTiered                 = false;

    struct Generation
    {

        llvm::orc::JITDylib *Dylib = nullptr;
        llvm::orc::ResourceTrackerSP Tracker;
        bool bLazy = false;
    };

    std::map<GenerationId, Generation> Generations;
    GenerationId NextGeneration = 1;

    llvm::orc::ThreadSafeContext Ctx;
    bool bContextAdopted = false;

    bool bRecordIr = false;
    std::string LastIrText;

    JitOptions Options;
    std::optional<Ir::IrGenerator> Gen;
    Ir::OwnedModules PendingReplacement;

    [[nodiscard]] Ir::IrOptions MakeIrOptions () const
    {
        Ir::IrOptions Opts;
        Opts.Granularity = Options.bPerUnitModules ? Ir::EModuleGranularity::PerUnit : Ir::EModuleGranularity::Whole;
        Opts.Tls         = Ir::ETlsAccess::Accessor;
        Opts.Linkage     = Options.bIndirectLinkage ? Ir::ELinkage::Indirect : Ir::ELinkage::Direct;

        Opts.SkipUnitsBelow             = Options.SkipUnitsBelow;
        Opts.bDefineInlineEligibleBelow = false;
        Opts.bDefineCompilerSeamUnits   = true;

        Opts.TargetTriple       = Jit == nullptr ? std::string{} : Jit->getTargetTriple().str();
        Opts.DataLayout         = Jit == nullptr ? std::string{} : Layout.getStringRepresentation();
        Opts.bNeedTargetMachine = false;

        Opts.EntryFunction = Options.EntryFunction;
        Opts.EntrySymbol   = Options.EntrySymbol;
        Opts.bVerify       = true;

        return Opts;
    }

    [[nodiscard]] Ir::IrOptions OneUnitOptions ( std::uint32_t Ordinal,
                                                 bool bReplacing,
                                                 const SymbolPredicate &IsAlreadyDefined,
                                                 const SymbolPredicate &HasIndirectionSlot ) const
    {
        Ir::IrOptions Opts            = MakeIrOptions();
        Opts.bReplaceUnit             = bReplacing;
        Opts.EntrySymbol              = {};
        Opts.bDefineSlotAccessor      = false;
        Opts.bDefineCompilerSeamUnits = false;
        Opts.SkipUnitsBelow           = bReplacing ? Opts.SkipUnitsBelow : Ordinal;
        Opts.IsAlreadyDefined         = IsAlreadyDefined;
        Opts.HasIndirectionSlot       = HasIndirectionSlot;
        return Opts;
    }

    [[nodiscard]] bool AddModules ( GenerationId GenId, Ir::OwnedModules Modules, std::string &OutError )
    {
        const auto Found = Generations.find( GenId );
        if ( Found == Generations.end() )
        {
            OutError = "jit: no such generation";
            return false;
        }
        if ( Modules.Modules.empty() )
        {
            OutError = "jit: the emission produced no module";
            return false;
        }

        if ( bRecordIr )
        {
            LastIrText.clear();
        }

        if ( Modules.Context != nullptr )
        {
            Ctx = llvm::orc::ThreadSafeContext( std::move( Modules.Context ) );
        }
        else if ( not bContextAdopted )
        {
            OutError = "jit: the first batch of modules brought no context";
            return false;
        }
        bContextAdopted = true;

        for ( std::unique_ptr<llvm::Module> &Mod : Modules.Modules )
        {
            if ( Mod == nullptr or not DefinesAnything( *Mod ) )
            {
                continue;
            }

            const std::string Name( Mod->getName() );
            DumpIfAsked( *Mod );
            if ( bRecordIr )
            {
                llvm::raw_string_ostream Text( LastIrText );
                Mod->print( Text, nullptr );
            }

            if ( Mod->getDataLayout().isDefault() )
            {
                Mod->setDataLayout( Layout );
            }
            else if ( Mod->getDataLayout() != Layout )
            {
                OutError = "jit: the module '" + Name + "' was typed for a different machine than the one running it";
                return false;
            }

            llvm::orc::ThreadSafeModule Safe( std::move( Mod ), Ctx );

            llvm::Error Err = Found->second.bLazy
                                  ? OnDemand->add( Found->second.Dylib == nullptr ? Jit->getMainJITDylib() : *Found->second.Dylib,
                                                   std::move( Safe ) )
                                  : Jit->addIRModule( Found->second.Tracker, std::move( Safe ) );
            if ( Err )
            {
                OutError = "jit: could not add the module '" + Name + "': " + Consume( std::move( Err ) );
                return false;
            }
        }
        return true;
    }

    void InstallPipeline ( llvm::OptimizationLevel Level )
    {
        Jit->getIRTransformLayer().setTransform(
            [Level] ( llvm::orc::ThreadSafeModule Tsm,
                      const llvm::orc::MaterializationResponsibility & ) -> llvm::Expected<llvm::orc::ThreadSafeModule>
            {
                Tsm.withModuleDo(
                    [Level] ( llvm::Module &Mod )
                    {
                        PinPromisedSymbols( Mod );
                        const std::optional<llvm::OptimizationLevel> Promoted = TierOf( Mod );
                        const Volt::Core::PhaseScope Timing( Promoted.has_value() ? "backend.jit.reoptimize"
                                                                                  : "backend.jit.optimize" );
                        RunPipeline( Mod, Promoted.value_or( Level ) );
                    } );
                return Tsm;
            } );
    }

    [[nodiscard]] bool BuildLazyStack ( std::uint8_t Tier )
    {
        llvm::orc::ExecutionSession &Session = Jit->getExecutionSession();
        const llvm::Triple &Machine          = Jit->getTargetTriple();

        llvm::Expected<std::unique_ptr<llvm::orc::LazyCallThroughManager>> LazyMgr = llvm::orc::createLocalLazyCallThroughManager(
            Machine, Session, llvm::orc::ExecutorAddr::fromPtr( &LazyCompileFailed ) );
        if ( not LazyMgr )
        {
            llvm::consumeError( LazyMgr.takeError() );
            return false;
        }

        llvm::orc::IRLayer &Below = Tier > 0 and InstallTiering( Tier )
                                        ? static_cast<llvm::orc::IRLayer &>( *ReOpt )
                                        : static_cast<llvm::orc::IRLayer &>( Jit->getIRTransformLayer() );

        CallThroughs = std::move( *LazyMgr );
        Partitions   = std::make_unique<llvm::orc::IRPartitionLayer>( Session, Below );
        Partitions->setPartitionFunction( PartitionWithCallees );
        OnDemand = std::make_unique<llvm::orc::CompileOnDemandLayer>(
            Session, *Partitions, *CallThroughs, llvm::orc::createLocalIndirectStubsManagerBuilder( Machine ) );
        return true;
    }

    [[nodiscard]] bool InstallTiering ( std::uint8_t Tier )
    {
        auto *Objects                       = llvm::dyn_cast<llvm::orc::ObjectLinkingLayer>( &Jit->getObjLinkingLayer() );
        llvm::orc::JITDylib *const Platform = Jit->getPlatformJITDylib().get();
        if ( Objects == nullptr or Platform == nullptr )
        {
            return false;
        }

        llvm::Expected<std::unique_ptr<llvm::orc::RedirectableSymbolManager>> RedirectMgr =
            llvm::orc::JITLinkRedirectableSymbolManager::Create( *Objects );
        if ( not RedirectMgr )
        {
            llvm::consumeError( RedirectMgr.takeError() );
            return false;
        }

        auto ReOptLayer = std::make_unique<llvm::orc::ReOptimizeLayer>( Jit->getExecutionSession(), Layout,
                                                                        Jit->getIRTransformLayer(), **RedirectMgr );

        if ( llvm::Error Err = ReOptLayer->addOrcRTLiteSupport( *Platform, Layout ) )
        {
            llvm::consumeError( std::move( Err ) );
            return false;
        }
        if ( llvm::Error Err = ReOptLayer->registerRuntimeFunctions( *Platform ) )
        {
            llvm::consumeError( std::move( Err ) );
            return false;
        }

        ReOptLayer->setReoptimizeFunc(
            [Tier] ( llvm::orc::ReOptimizeLayer &, llvm::orc::ReOptimizeLayer::ReOptMaterializationUnitID, unsigned,
                     llvm::orc::ResourceTrackerSP, llvm::orc::ThreadSafeModule &Tsm ) -> llvm::Error
            {
                Tsm.withModuleDo( [Tier] ( llvm::Module &Mod ) { Mod.setModuleFlag( llvm::Module::Override, TierFlag, Tier ); } );
                return llvm::Error::success();
            } );

        ReOptLayer->setAddProfilerFunc( ProfileIfCandidate );

        Redirects = std::move( *RedirectMgr );
        ReOpt     = std::move( ReOptLayer );
        bTiered   = true;
        return true;
    }
};

Volt::Backend::Jit::OrcJitQueue::OrcJitQueue () : P( std::make_unique<Impl>() )
{
}

Volt::Backend::Jit::OrcJitQueue::~OrcJitQueue ()                                                       = default;
Volt::Backend::Jit::OrcJitQueue::OrcJitQueue ( OrcJitQueue && ) noexcept                               = default;
Volt::Backend::Jit::OrcJitQueue &Volt::Backend::Jit::OrcJitQueue::operator=( OrcJitQueue && ) noexcept = default;

std::string Volt::Backend::Jit::OrcJitQueue::TargetTriple () const
{
    return P->Jit == nullptr ? std::string{} : P->Jit->getTargetTriple().str();
}

std::string Volt::Backend::Jit::OrcJitQueue::DataLayoutString () const
{
    return P->Jit == nullptr ? std::string{} : P->Layout.getStringRepresentation();
}

bool Volt::Backend::Jit::OrcJitQueue::Init ( const SessionOptions &Wanted, std::string &OutError )
{
    InitialiseNativeTarget();

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

    const bool bLazy = Wanted.Policy == ECompilePolicy::Lazy and P->BuildLazyStack( Wanted.OptLevel );
    P->CompilePolicy = bLazy ? ECompilePolicy::Lazy : ECompilePolicy::Eager;

    P->InstallPipeline( Ir::OptimizationLevelOf( P->bTiered ? std::uint8_t{ 0 } : Wanted.OptLevel ) );
    return true;
}

Volt::Backend::Jit::ECompilePolicy Volt::Backend::Jit::OrcJitQueue::Policy () const
{
    return P->CompilePolicy;
}

bool Volt::Backend::Jit::OrcJitQueue::Tiering () const
{
    return P->bTiered;
}

bool Volt::Backend::Jit::OrcJitQueue::AddDylib ( std::string_view Path, std::string &OutError )
{
    const char Prefix = P->Layout.getGlobalPrefix();

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

bool Volt::Backend::Jit::OrcJitQueue::AddProcessSymbols ( std::string &OutError )
{
    const char Prefix = P->Layout.getGlobalPrefix();

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

bool Volt::Backend::Jit::OrcJitQueue::Lookup ( std::string_view Symbol, std::uintptr_t &OutAddr, std::string &OutError )
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

bool Volt::Backend::Jit::OrcJitQueue::LookupIn ( GenerationId GenId,
                                                 std::string_view Symbol,
                                                 std::uintptr_t &OutAddr,
                                                 std::string &OutError )
{
    const auto Found = P->Generations.find( GenId );
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

Volt::Backend::Jit::GenerationId Volt::Backend::Jit::OrcJitQueue::OpenGeneration ()
{
    const GenerationId Id = P->NextGeneration++;
    Impl::Generation &Gen = P->Generations[Id];
    Gen.bLazy             = P->CompilePolicy == ECompilePolicy::Lazy;
    if ( not Gen.bLazy )
    {
        Gen.Tracker = P->Jit->getMainJITDylib().createResourceTracker();
    }
    return Id;
}

bool Volt::Backend::Jit::OrcJitQueue::OpenReplacement ( GenerationId &OutGen, std::string &OutError )
{
    const GenerationId Id = P->NextGeneration++;

    llvm::Expected<llvm::orc::JITDylib &> Made = P->Jit->createJITDylib( "volt.gen." + std::to_string( Id ) );
    if ( not Made )
    {
        OutError = "jit: could not open a generation dylib: " + Consume( Made.takeError() );
        return false;
    }

    Made->addToLinkOrder( P->Jit->getMainJITDylib() );
    P->Jit->getMainJITDylib().addToLinkOrder( *Made );

    Impl::Generation &Gen = P->Generations[Id];
    Gen.Dylib             = &*Made;
    Gen.Tracker           = Made->createResourceTracker();
    Gen.bLazy             = false;

    OutGen = Id;
    return true;
}

bool Volt::Backend::Jit::OrcJitQueue::DropGeneration ( GenerationId GenId, std::string &OutError )
{
    const auto Found = P->Generations.find( GenId );
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

std::size_t Volt::Backend::Jit::OrcJitQueue::LiveGenerations () const
{
    return P->Generations.size();
}

void Volt::Backend::Jit::OrcJitQueue::Begin ( const BackendInput &Input, const JitOptions &Options )
{
    P->Options = Options;
    P->Gen.emplace( P->MakeIrOptions() );
    P->Gen->Begin( Input );
}

Volt::Backend::EEmitStatus Volt::Backend::Jit::OrcJitQueue::EmitUnit ( const UnitView &Unit, CompiledUnitMeta &OutMeta )
{
    if ( not P->Gen )
    {
        return EEmitStatus::Error;
    }

    const EEmitStatus Status = P->Gen->EmitUnit( Unit );
    HarvestMeta( *P->Gen, OutMeta );
    return Status;
}

Volt::Backend::EmitResult Volt::Backend::Jit::OrcJitQueue::Finalize ( GenerationId GenId )
{
    if ( not P->Gen )
    {
        return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = "jit: queue was not begun" };
    }

    const EEmitStatus Finished = P->Gen->Finish();
    if ( Finished != EEmitStatus::Ok )
    {
        return EmitResult{ .Status = Finished, .Artifact = {}, .Message = std::string( P->Gen->Error() ) };
    }

    std::string Error;
    const Volt::Core::PhaseScope Timing( "backend.jit.add" );
    if ( not P->AddModules( GenId, Ir::TakeModules( *P->Gen ), Error ) )
    {
        return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = Error };
    }

    return EmitResult{ .Status = EEmitStatus::Ok, .Artifact = "<jit>", .Message = {} };
}

std::size_t Volt::Backend::Jit::OrcJitQueue::UnwindStorageSize () const
{
    return P->Gen ? P->Gen->UnwindStorageSize() : 0;
}

bool Volt::Backend::Jit::OrcJitQueue::PrepareReplacement ( const BackendInput &Build,
                                                           const UnitView &Unit,
                                                           const SymbolPredicate &IsAlreadyDefined,
                                                           const SymbolPredicate &HasIndirectionSlot,
                                                           CompiledUnitMeta &OutMeta,
                                                           std::string &OutError )
{
    Ir::IrGenerator Replacement( P->OneUnitOptions( Unit.Ordinal, /*bReplacing=*/true, IsAlreadyDefined, HasIndirectionSlot ) );
    Replacement.Begin( Build );
    if ( Replacement.EmitUnit( Unit ) != EEmitStatus::Ok or Replacement.Finish() != EEmitStatus::Ok )
    {
        OutError = "jit: replacement unit did not emit: " + std::string( Replacement.Error() );
        return false;
    }

    HarvestMeta( Replacement, OutMeta );
    P->PendingReplacement = Ir::TakeModules( Replacement );
    return true;
}

bool Volt::Backend::Jit::OrcJitQueue::CommitReplacement ( GenerationId GenId, std::string &OutError )
{
    const Volt::Core::PhaseScope Timing( "backend.jit.add" );
    return P->AddModules( GenId, std::move( P->PendingReplacement ), OutError );
}

void Volt::Backend::Jit::OrcJitQueue::DiscardReplacement ()
{
    P->PendingReplacement.Modules.clear();
    P->PendingReplacement.Context.reset();
}

bool Volt::Backend::Jit::OrcJitQueue::CompileEvalUnit ( const BackendInput &Build,
                                                        const UnitView &Unit,
                                                        GenerationId &OutGen,
                                                        bool &OutRedefines,
                                                        const SymbolPredicate &IsAlreadyDefined,
                                                        const SymbolPredicate &HasIndirectionSlot,
                                                        CompiledUnitMeta &OutMeta,
                                                        std::size_t BootUnwindStorage,
                                                        std::string &OutError )
{
    Ir::IrGenerator Line( P->OneUnitOptions( Unit.Ordinal, false, IsAlreadyDefined, HasIndirectionSlot ) );
    Line.Begin( Build );
    if ( Line.EmitUnit( Unit ) != EEmitStatus::Ok or Line.Finish() != EEmitStatus::Ok )
    {
        OutError = "jit: evaluated unit did not emit: " + std::string( Line.Error() );
        return false;
    }

    if ( Line.UnwindStorageSize() > BootUnwindStorage )
    {
        OutError = "repl: this line can raise a value of " + std::to_string( Line.UnwindStorageSize() ) +
                   " bytes, wider than the session's unwind buffer of " + std::to_string( BootUnwindStorage ) +
                   " bytes, which was fixed when the session started.\n"
                   "       -> :reset reopens a session sized for it, or declare the type in a file loaded at startup.";
        return false;
    }

    HarvestMeta( Line, OutMeta );

    const bool bRedefines = std::any_of( OutMeta.Symbols.begin(), OutMeta.Symbols.end(),
                                         [&IsAlreadyDefined] ( const auto &Symbol ) { return IsAlreadyDefined( Symbol.Name ); } );
    OutRedefines          = bRedefines;

    GenerationId Gen = 0;
    if ( bRedefines )
    {
        if ( not OpenReplacement( Gen, OutError ) )
        {
            return false;
        }
    }
    else
    {
        Gen = OpenGeneration();
    }

    OutGen = Gen;
    const Volt::Core::PhaseScope Timing( "backend.jit.add" );
    return P->AddModules( Gen, Ir::TakeModules( Line ), OutError );
}

bool Volt::Backend::Jit::OrcJitQueue::CompileBenchUnit ( const BackendInput &Build,
                                                         const UnitView &Unit,
                                                         GenerationId &OutGen,
                                                         const SymbolPredicate &IsAlreadyDefined,
                                                         const SymbolPredicate &HasIndirectionSlot,
                                                         std::string &OutError )
{
    GenerationId Gen = 0;
    if ( not OpenReplacement( Gen, OutError ) )
    {
        return false;
    }

    Ir::IrGenerator Line( P->OneUnitOptions( Unit.Ordinal, true, IsAlreadyDefined, HasIndirectionSlot ) );
    Line.Begin( Build );
    if ( Line.EmitUnit( Unit ) != EEmitStatus::Ok or Line.Finish() != EEmitStatus::Ok )
    {
        OutError = "jit: bench unit did not emit: " + std::string( Line.Error() );
        std::string DropErr;
        ( void )DropGeneration( Gen, DropErr );
        return false;
    }

    OutGen = Gen;
    const Volt::Core::PhaseScope Timing( "backend.jit.add" );
    if ( not P->AddModules( Gen, Ir::TakeModules( Line ), OutError ) )
    {
        std::string DropErr;
        ( void )DropGeneration( Gen, DropErr );
        return false;
    }
    return true;
}

bool Volt::Backend::Jit::OrcJitQueue::ProbeUnit ( const BackendInput &Build,
                                                  const UnitView &Unit,
                                                  std::string *OutIr,
                                                  std::string &OutError )
{
    Ir::IrGenerator Line( P->OneUnitOptions( Unit.Ordinal, false, {}, {} ) );
    Line.Begin( Build );
    if ( Line.EmitUnit( Unit ) != EEmitStatus::Ok or Line.Finish() != EEmitStatus::Ok )
    {
        OutError = "jit: this line did not emit: " + std::string( Line.Error() );
        return false;
    }

    if ( OutIr != nullptr )
    {
        const Ir::OwnedModules Emitted = Ir::TakeModules( Line );
        const auto DefinesABody        = [] ( const llvm::Module &Mod )
        {
            for ( const llvm::Function &Fn : Mod )
            {
                if ( not Fn.isDeclaration() )
                {
                    return true;
                }
            }
            return false;
        };

        llvm::raw_string_ostream Text( *OutIr );
        for ( const std::unique_ptr<llvm::Module> &Mod : Emitted.Modules )
        {
            if ( Mod != nullptr and DefinesABody( *Mod ) )
            {
                Mod->print( Text, nullptr );
            }
        }
    }
    return true;
}

void Volt::Backend::Jit::OrcJitQueue::RecordIr ( bool bEnable )
{
    P->bRecordIr = bEnable;
}

std::string Volt::Backend::Jit::OrcJitQueue::LastIr () const
{
    return P->LastIrText;
}

std::string
Volt::Backend::Jit::OrcJitQueue::Disassemble ( std::uintptr_t Address, std::size_t MaxBytes, std::string &OutError ) const
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

std::unique_ptr<Volt::Backend::Jit::IJitQueue> Volt::Backend::Jit::CreateOrcJitQueue ()
{
    return std::make_unique<OrcJitQueue>();
}
