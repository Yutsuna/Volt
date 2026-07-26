// LlvmEmitter.cpp — LlvmBackend's lifecycle and the two sweeps over the units.
//
// The emission itself lives in sibling TUs (TypeMapper, ExprEmitter, ...);
// this file owns only the shape of a build: set up the module and the host
// target, declare everything reachable, then define it.

#include "LlvmState.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FileUtilities.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

#include <mutex>
#include <string>
#include <utility>
#include <variant>

// The concept is the contract; breaking the signature is a compile error here,
// not a discovery made at the Driver's runtime seam.
static_assert( Volt::Backend::TargetBackend<Volt::Backend::Llvm::LlvmBackend> );

namespace
{

// LLVM's target registry is process-global and not re-entrant. Volt compiles
// units in parallel elsewhere, so the initialisation is guarded even though
// codegen itself is single-threaded.
void InitialiseNativeTarget ()
{
    static std::once_flag Once;
    std::call_once( Once,
                    [] ()
                    {
                        llvm::InitializeNativeTarget();
                        llvm::InitializeNativeTargetAsmPrinter();
                        llvm::InitializeNativeTargetAsmParser();
                    } );
}

} // namespace

Volt::Backend::EEmitStatus Volt::Backend::Llvm::LlvmBackend::State::Fail ( std::string InMessage )
{
    if ( Status != EEmitStatus::Error )
    {
        Status = EEmitStatus::Error;
        // The symbol being emitted, appended once here rather than at ~60 call
        // sites: every one of these messages names a *middle-end* fact that is
        // missing, and the first question about any of them is "in which body".
        // The name is the mangled one, which is exactly owner + method.
        Message = std::move( InMessage );
        if ( Frame.Fn != nullptr )
        {
            Message += " (while emitting '" + Frame.Fn->getName().str() + "')";
        }
    }
    return EEmitStatus::Error;
}

bool Volt::Backend::Llvm::LlvmBackend::State::InitTarget ( std::string_view ModuleName )
{
    InitialiseNativeTarget();

    Mod     = std::make_unique<llvm::Module>( ModuleName, Context );
    Builder = std::make_unique<llvm::IRBuilder<>>( Context );

    // One string is the whole cross-compilation seam.
    const std::string TripleText = llvm::sys::getDefaultTargetTriple();
    const llvm::Triple Triple{ TripleText };
    Mod->setTargetTriple( Triple );

    std::string Error;
    const llvm::Target *Target = llvm::TargetRegistry::lookupTarget( Triple, Error );
    if ( Target == nullptr )
    {
        static_cast<void>( Fail( "llvm: no target for host triple '" + TripleText + "': " + Error ) );
        return false;
    }

    // PIC, not the default static model: every mainstream toolchain links
    // PIE by default, and a static-model object hits `R_X86_64_32 ... can not
    // be used; recompile with -fPIC` at the link, which is a failure with no
    // relation to anything in the program. The C driver Linker.cpp shells out
    // to is the same one that made that choice, so matching it here is what
    // keeps the two halves of the build agreeing.
    Machine.reset( Target->createTargetMachine( Triple, "generic", "", llvm::TargetOptions{}, llvm::Reloc::PIC_ ) );
    if ( Machine == nullptr )
    {
        static_cast<void>( Fail( "llvm: could not create a TargetMachine for '" + TripleText + "'" ) );
        return false;
    }

    // Set before any type is created: the ABI cross-check in TypeMapper reads
    // struct offsets out of this DataLayout and compares them against
    // LayoutEngine, so an unset layout would make the check vacuous.
    Mod->setDataLayout( Machine->createDataLayout() );
    return true;
}

llvm::Type *Volt::Backend::Llvm::LlvmBackend::State::ParamTypeOfLayout ( Sema::LayoutId Id )
{
    llvm::Type *Shape = TypeOfLayout( Id );
    if ( Shape == nullptr )
    {
        return nullptr;
    }
    return Shape->isStructTy() ? llvm::PointerType::get( Context, 0 ) : Shape;
}

Volt::Sema::LayoutId Volt::Backend::Llvm::LlvmBackend::State::SignatureLayoutOf ( Sema::TypeStore &Store,
                                                                                  Sema::SigTypeId Id,
                                                                                  Sema::NominalId Owner,
                                                                                  std::span<const std::uint32_t> FlatArgs )
{
    if ( Id.IsValid() and Store.Sig( Id ).ParamIndex == Sema::SigType::SelfParam )
    {
        return Instances.Of( Store, Owner, FlatArgs );
    }
    return Instances.OfSignature( Store, Id, FlatArgs );
}

llvm::FunctionType *Volt::Backend::Llvm::LlvmBackend::State::FunctionTypeOf ( const Sema::Member &Entry,
                                                                              Sema::NominalId Owner,
                                                                              std::span<const std::uint32_t> FlatArgs )
{
    Sema::TypeStore &Store = *Build->Types;

    std::vector<llvm::Type *> Params;
    Params.reserve( Entry.Params.Size() + 1 );

    // `self` leads — except for a static `def self.x`, which has no receiver,
    // and for an @[External] member, whose signature is a C prototype and must
    // carry exactly what the C declaration wrote.
    const bool bExternal = Entry.ExternSymbol.IsValid();
    if ( Owner.IsValid() and not Entry.bSelf and not bExternal )
    {
        llvm::Type *Self = ParamTypeOfLayout( Instances.Of( Store, Owner, FlatArgs ) );
        if ( Self == nullptr )
        {
            static_cast<void>(
                Fail( "llvm: receiver of '" + std::string( Store.Text( Entry.Name ) ) + "' has no resolved layout" ) );
            return nullptr;
        }
        Params.push_back( Self );
    }

    for ( std::size_t Index = 0; Index < Entry.Params.Size(); ++Index )
    {
        // A `&block` parameter carries a closure, and a closure value is
        // uniformly the two-slot `{ code, env }` aggregate (abi.md) — an
        // aggregate, hence a pointer, whatever the block's own signature says.
        if ( Index < Entry.ParamIsBlock.Size() and Entry.ParamIsBlock[Index] )
        {
            Params.push_back( llvm::PointerType::get( Context, 0 ) );
            continue;
        }

        llvm::Type *Slot = ParamTypeOfLayout( SignatureLayoutOf( Store, Entry.Params[Index], Owner, FlatArgs ) );
        if ( Slot == nullptr )
        {
            static_cast<void>( Fail( "llvm: parameter " + std::to_string( Index ) + " of '" +
                                     std::string( Store.Text( Entry.Name ) ) + "' has no resolved layout" ) );
            return nullptr;
        }
        Params.push_back( Slot );
    }

    // No result signature means no return type: a `def` with no `-> T` has
    // none (rules/core-ast.md), and that is the same shape as a declared
    // return whose nominal the stdlib never defines. Both are `void`; neither
    // is guessed at.
    llvm::Type *Result =
        Entry.Result.IsValid() ? TypeOfLayout( SignatureLayoutOf( Store, Entry.Result, Owner, FlatArgs ) ) : nullptr;
    if ( Result == nullptr )
    {
        Result = llvm::Type::getVoidTy( Context );
    }

    return llvm::FunctionType::get( Result, Params, false );
}

std::string Volt::Backend::Llvm::LlvmBackend::State::SymbolOf ( const Sema::Member &Entry,
                                                                Sema::NominalId Owner,
                                                                std::span<const std::uint32_t> FlatArgs ) const
{
    // An @[External] member never enters the mangling scheme: the whole point
    // of that boundary is that the linker and a C compiler agree on the name,
    // so the recorded C spelling is used verbatim.
    if ( Entry.ExternSymbol.IsValid() )
    {
        return std::string( Build->Types->Text( Entry.ExternSymbol ) );
    }
    return MangleFunction( *Build->Types, Entry, Owner, FlatArgs );
}

llvm::Function *Volt::Backend::Llvm::LlvmBackend::State::FunctionFor ( const Sema::Member &Entry,
                                                                       Sema::NominalId Owner,
                                                                       std::span<const std::uint32_t> FlatArgs )
{
    const std::string Symbol = SymbolOf( Entry, Owner, FlatArgs );
    if ( const auto It = Functions.find( Symbol ); It != Functions.end() )
    {
        return It->second;
    }

    llvm::FunctionType *Signature = FunctionTypeOf( Entry, Owner, FlatArgs );
    if ( Signature == nullptr )
    {
        return nullptr;
    }

    llvm::Function *Fn = llvm::Function::Create( Signature, llvm::Function::ExternalLinkage, Symbol, Mod.get() );
    Functions.emplace( Symbol, Fn );
    return Fn;
}

llvm::Function *Volt::Backend::Llvm::LlvmBackend::State::DeclareMember ( const Sema::Member &Entry, Sema::NominalId Owner )
{
    // A field is storage, not code. An `abstract def` is a contract: either an
    // including type overrides it, or its receiver's layout exempts it and the
    // backend supplies an instruction — no symbol either way.
    if ( Entry.Kind != Sema::EMemberKind::Method or Entry.bAbstract )
    {
        return nullptr;
    }

    // A method with its own generics has no signature until a call site fixes
    // them; it is emitted by the monomorphiser, from a MonoRequest.
    if ( Entry.OwnGenerics > 0 )
    {
        return nullptr;
    }

    return FunctionFor( Entry, Owner, {} );
}

bool Volt::Backend::Llvm::LlvmBackend::State::IsMixinOwner ( Sema::NominalId Id ) const
{
    if ( not Id.IsValid() or Build == nullptr )
    {
        return false;
    }

    const Sema::TypeStore &Store  = *Build->Types;
    const Sema::NominalType &Type = Store.Type( Id );
    for ( const UnitView &View : Build->Units )
    {
        if ( View.Ordinal == Type.Unit and View.Ast != nullptr )
        {
            return std::holds_alternative<Frontend::Mixin>( View.Ast->Decl( Type.Decl ) );
        }
    }
    return false;
}

void Volt::Backend::Llvm::LlvmBackend::State::DeclareAll ()
{
    Sema::TypeStore &Store = *Build->Types;

    // The TypeStore is the declare sweep's input, not the ASTs: it is the
    // build-wide, already-resolved interface of every unit, so one pass over
    // it covers all units at once and a DeclId — meaningful only inside the
    // arena that minted it — never has to leave its unit.
    for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
    {
        const Sema::NominalId Id{ static_cast<Sema::NominalId::ValueType>( Index ) };

        // A generic type's members have no shape until its arguments are
        // fixed; `Array<Int32>#push` is minted by the monomorphiser instead.
        // A mixin's own concrete methods are generic over `self` in exactly
        // the same sense, and no less so for declaring zero `<T>`s of its
        // own — IsMixinOwner is the check that catches it (see LlvmState.hpp).
        if ( Store.Type( Id ).Params.Size() > 0 or IsMixinOwner( Id ) )
        {
            continue;
        }

        for ( const Sema::Member &Entry : Store.Type( Id ).Members )
        {
            static_cast<void>( DeclareMember( Entry, Id ) );
        }
    }

    for ( const Sema::Member &Entry : Store.FreeFunctions() )
    {
        static_cast<void>( DeclareMember( Entry, Sema::NominalId{} ) );
    }

    if ( Build != nullptr )
    {
        llvm::FunctionType *InitFnTy = llvm::FunctionType::get( Builder->getVoidTy(), false );
        for ( const UnitView &Unit : Build->Units )
        {
            const std::string InitName = "_V_init_" + std::to_string( Unit.Ordinal );
            if ( Mod->getFunction( InitName ) == nullptr )
            {
                llvm::Function::Create( InitFnTy, llvm::Function::ExternalLinkage, InitName, Mod.get() );
            }
        }
    }
}

void Volt::Backend::Llvm::LlvmBackend::State::DefineMember ( const Sema::Member &Entry,
                                                             Sema::NominalId Owner,
                                                             const UnitView &Unit )
{
    // The same four exclusions the declare sweep applies, plus @[External]:
    // that one *has* a symbol — it is declared, and calls to it link — but its
    // body lives outside Volt, so there is nothing here to emit.
    if ( Entry.Kind != Sema::EMemberKind::Method or Entry.bAbstract or Entry.OwnGenerics > 0 or Entry.ExternSymbol.IsValid() )
    {
        return;
    }

    const auto *Node = std::get_if<Frontend::Method>( &Unit.Ast->Decl( Entry.Decl ) );
    if ( Node == nullptr )
    {
        static_cast<void>( Fail( "llvm: the store calls '" + std::string( Build->Types->Text( Entry.Name ) ) +
                                 "' a method, but its declaration in " + std::string( Unit.Path ) + " is not one" ) );
        return;
    }

    llvm::Function *Fn = FunctionFor( Entry, Owner, {} );
    if ( Fn == nullptr or not Fn->empty() )
    {
        return;
    }

    Sema::TypeStore &Store = *Build->Types;

    // A fresh frame per function: a slot left over from the previous body
    // would resolve a name to storage that no longer exists.
    Frame               = FunctionFrame{};
    Frame.Fn            = Fn;
    Frame.Unit          = &Unit;
    Frame.Values        = Unit.Values;
    Frame.Callees       = Unit.Callees;
    Frame.Owner         = Owner;
    Frame.Entry         = llvm::BasicBlock::Create( Context, "entry", Fn );
    Frame.bReturnsValue = not Fn->getReturnType()->isVoidTy();
    Builder->SetInsertPoint( Frame.Entry );

    // Parameter order is abi.md's, and it is read here exactly as
    // FunctionTypeOf wrote it — the two are one contract, so `self` leads under
    // the same condition in both.
    unsigned Index = 0;
    if ( Owner.IsValid() and not Entry.bSelf )
    {
        Frame.SelfLayout = Instances.Of( Store, Owner, {} );
        Frame.Self       = Fn->getArg( 0 );
        Frame.Self->setName( "self" );
        Index = 1;
    }

    for ( const Frontend::ParamId ParamRef : Node->Params )
    {
        llvm::Value *Arg                = Fn->getArg( Index++ );
        const Frontend::Param &Declared = Unit.Ast->GetParam( ParamRef );
        Arg->setName( Unit.Ast->Text( Declared.Name ) );

        const Sema::BindingSite Site{ ParamRef };
        Frame.Slots.emplace( Site, Arg );

        BindInstanceVarParam( ParamRef, Arg );
    }

    EmitStmts( Node->Body, Frame.bReturnsValue );

    // Return unit's zero value if control flows off the end of a non-void
    // method without an explicit `return` / tail expression — rules/core-ast.md
    // guarantees Sema checked every path returns, so reaching here means the
    // end of the body is unreachable, but LLVM IR requires every basic block to
    // be terminated anyway.
    if ( not Terminated() )
    {
        if ( Frame.bReturnsValue )
        {
            static_cast<void>( Builder->CreateRet( llvm::ConstantInt::get( Fn->getReturnType(), 0 ) ) );
        }
        else
        {
            static_cast<void>( Builder->CreateRetVoid() );
        }
    }

    Frame = FunctionFrame{};
}

void Volt::Backend::Llvm::LlvmBackend::State::BindInstanceVarParam ( Frontend::ParamId ParamRef, llvm::Value *Value )
{
    const Frontend::Param &Declared = Frame.Unit->Ast->GetParam( ParamRef );

    // `def initialize( @x : Int32 )` declares a parameter *and* stores it into
    // the field of that name — the parser records which (`Param::bInstanceVar`,
    // with the sigil already stripped) and no pass materialises the store, so
    // it is emitted here as part of binding the parameter. Without it every
    // field so declared stays whatever the frame happened to hold, silently:
    // the stdlib's `Exception#initialize( @message : String )` and any
    // `Point.new( 3, 4 )` both depend on it.
    if ( not Declared.bInstanceVar or Value == nullptr )
    {
        return;
    }
    if ( Frame.Self == nullptr )
    {
        static_cast<void>( Fail( "llvm: '@" + std::string( Frame.Unit->Ast->Text( Declared.Name ) ) +
                                 "' is a field-assigning parameter of a method with no receiver" ) );
        return;
    }

    llvm::Value *Address =
        FieldAddress( Frame.Self, Frame.SelfLayout, Frame.Unit->Ast->Text( Declared.Name ), Frontend::ExprId{} );
    if ( Address == nullptr )
    {
        return;
    }
    EmitStore( Address, Value, LayoutOfValue( *Frame.Values, Frame.Values->SiteType( Sema::BindingSite{ ParamRef } ) ) );
}

bool Volt::Backend::Llvm::LlvmBackend::State::EmitEntryPoint ()
{
    // The C entry symbol (e.g. `main`). If no entry symbol is requested or if
    // one is already defined in the LLVM module, skip emitting the entry point.
    if ( Options.EntrySymbol.empty() or Mod->getFunction( Options.EntrySymbol ) != nullptr )
    {
        return true;
    }

    llvm::Type *ExitCodeTy = llvm::Type::getInt32Ty( Context );
    llvm::Type *ArgcTy     = llvm::Type::getInt32Ty( Context );
    llvm::Type *ArgvTy     = llvm::PointerType::getUnqual( Context );

    llvm::FunctionType *MainTy = llvm::FunctionType::get( ExitCodeTy, { ArgcTy, ArgvTy }, false );
    llvm::Function *MainFn = llvm::Function::Create( MainTy, llvm::Function::ExternalLinkage, Options.EntrySymbol, Mod.get() );

    llvm::IRBuilder<> Shell{ llvm::BasicBlock::Create( Context, "entry", MainFn ) };

    if ( Build != nullptr )
    {
        for ( const UnitView &Unit : Build->Units )
        {
            const std::string InitName = "_V_init_" + std::to_string( Unit.Ordinal );
            llvm::Function *InitFn     = Mod->getFunction( InitName );
            if ( InitFn == nullptr )
            {
                llvm::FunctionType *FnTy = llvm::FunctionType::get( Builder->getVoidTy(), false );
                InitFn                   = llvm::Function::Create( FnTy, llvm::Function::ExternalLinkage, InitName, Mod.get() );
            }
            static_cast<void>( Shell.CreateCall( InitFn ) );
        }
    }

    static_cast<void>( Shell.CreateRet( llvm::ConstantInt::get( ExitCodeTy, 0 ) ) );
    return true;
}

void Volt::Backend::Llvm::LlvmBackend::State::EmitUnitInit ( const UnitView &Unit )
{
    if ( Unit.Ast == nullptr )
    {
        return;
    }

    const std::string InitName = "_V_init_" + std::to_string( Unit.Ordinal );
    llvm::Function *InitFn     = Mod->getFunction( InitName );
    if ( InitFn == nullptr )
    {
        llvm::FunctionType *FnTy = llvm::FunctionType::get( Builder->getVoidTy(), false );
        InitFn                   = llvm::Function::Create( FnTy, llvm::Function::ExternalLinkage, InitName, Mod.get() );
    }

    Frame               = FunctionFrame{};
    Frame.Fn            = InitFn;
    Frame.Unit          = &Unit;
    Frame.Values        = Unit.Values;
    Frame.Callees       = Unit.Callees;
    Frame.Entry         = llvm::BasicBlock::Create( Context, "entry", InitFn );
    Frame.bReturnsValue = false;
    Builder->SetInsertPoint( Frame.Entry );

    for ( const Frontend::StmtId StmtId : Unit.Ast->TopStmts )
    {
        EmitStmt( StmtId, /*bTail=*/false );
    }

    if ( not Terminated() )
    {
        static_cast<void>( Builder->CreateRetVoid() );
    }

    Frame = FunctionFrame{};
}

void Volt::Backend::Llvm::LlvmBackend::State::DefineAll ( const UnitView &Unit )
{
    Sema::TypeStore &Store = *Build->Types;

    // Symmetric with DeclareAll, and for the same reason: the store is the
    // resolved interface of the whole build, so the sweep asks "which of these
    // members does *this* unit hold a body for" rather than walking a Decl
    // arena and searching the store back. `Member::Unit` is the declaring
    // unit's *discovery* ordinal, which is what UnitView::Ordinal carries —
    // deliberately not the view's index, since the views are in circuit link
    // order and the two diverge as soon as a circuit has edges.
    for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
    {
        const Sema::NominalId Id{ static_cast<Sema::NominalId::ValueType>( Index ) };
        if ( Store.Type( Id ).Params.Size() > 0 or IsMixinOwner( Id ) )
        {
            continue;
        }

        for ( const Sema::Member &Entry : Store.Type( Id ).Members )
        {
            if ( Entry.Unit == Unit.Ordinal )
            {
                DefineMember( Entry, Id, Unit );
            }
        }
    }

    for ( const Sema::Member &Entry : Store.FreeFunctions() )
    {
        if ( Entry.Unit == Unit.Ordinal )
        {
            DefineMember( Entry, Sema::NominalId{}, Unit );
        }
    }

    EmitUnitInit( Unit );
}

Volt::Backend::Llvm::LlvmBackend::LlvmBackend () : Impl( std::make_unique<State>() )
{
}

Volt::Backend::Llvm::LlvmBackend::~LlvmBackend () = default;

Volt::Backend::Llvm::LlvmBackend::LlvmBackend ( LlvmBackend && ) noexcept = default;

Volt::Backend::Llvm::LlvmBackend &Volt::Backend::Llvm::LlvmBackend::operator=( LlvmBackend && ) noexcept = default;

void Volt::Backend::Llvm::LlvmBackend::SetOptions ( EmitOptions InOptions )
{
    Impl->Options = std::move( InOptions );
}

void Volt::Backend::Llvm::LlvmBackend::Begin ( const BackendInput &Input )
{
    Impl->Build = &Input;

    if ( Input.Types == nullptr )
    {
        static_cast<void>( Impl->Fail( "llvm: the build carries no TypeStore" ) );
        return;
    }

    Impl->Layouts.emplace( *Input.Types );

    // One llvm::Module per build: the simplest correct thing. Per-unit modules
    // plus ThinLTO is a later optimisation behind this same interface. The
    // entry module is last in circuit link order, so it names the module.
    const std::string_view Name = Input.Units.empty() ? std::string_view{ "volt" } : Input.Units.back().Module;
    if ( not Impl->InitTarget( Name ) )
    {
        return;
    }

    // Declare before defining anything: a body emitted in the first unit may
    // call something declared in the last, and one pass over the store means
    // that resolves immediately instead of needing a fixup pass.
    Impl->DeclareAll();
}

Volt::Backend::EEmitStatus Volt::Backend::Llvm::LlvmBackend::EmitUnit ( const UnitView &Unit )
{
    if ( Impl->Failed() )
    {
        return EEmitStatus::Error;
    }

    if ( Unit.Ast == nullptr or Unit.Values == nullptr or Unit.Callees == nullptr or Unit.Scopes == nullptr )
    {
        return Impl->Fail( "llvm: unit '" + std::string( Unit.Path ) + "' reached the backend with no sema output" );
    }

    Impl->DefineAll( Unit );
    return Impl->Failed() ? EEmitStatus::Error : EEmitStatus::Ok;
}

Volt::Backend::EmitResult Volt::Backend::Llvm::LlvmBackend::Finalize ()
{
    const auto MakeFailure = [this] ()
    { return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = Impl->Message }; };

    if ( Impl->Failed() )
    {
        return MakeFailure();
    }

    // Every unit is defined by now, so every instantiation a concrete body
    // could ever discover has been enqueued. A drained body can itself
    // enqueue more — a generic method calling another generic method — so
    // this drains to a fixpoint rather than once.
    Impl->DrainMonomorphizer();
    if ( Impl->Failed() )
    {
        return MakeFailure();
    }

    // After the drain, so the entry point can itself be the thing that forced
    // an instantiation, and before the verifier, which is what proves the shim
    // is well formed like any other function.
    if ( not Impl->EmitEntryPoint() )
    {
        return MakeFailure();
    }

    // A broken module past this point is an emitter bug (rules/core-ast.md
    // guarantees the middle-end never hands over anything the module
    // verifier could reject), never a Volt source error — VerifyModule names
    // the offending function rather than the caller guessing.
    if ( not Impl->VerifyModule() )
    {
        return MakeFailure();
    }

    // Runs even at -O0: PassBuilder's O0 pipeline is the minimal
    // semantically-required set, principally mem2reg, and the emitter itself
    // never builds SSA (llvm.md, "every alloca goes in the entry block") —
    // it depends on this pass to promote them back to registers.
    Impl->RunOptimizationPipeline();

    const std::string ModuleName = Impl->Mod->getName().str();
    const std::string BaseName   = ModuleName.empty() ? std::string( "volt" ) : ModuleName;

    if ( Impl->Options.Stage == EEmitStage::Ir )
    {
        const std::string Path = Impl->Options.OutputPath.empty() ? BaseName + ".ll" : Impl->Options.OutputPath;
        if ( not Impl->EmitIrFile( Path ) )
        {
            return MakeFailure();
        }
        return EmitResult{ .Status = EEmitStatus::Ok, .Artifact = Path, .Message = {} };
    }

    if ( Impl->Options.Stage == EEmitStage::Object )
    {
        const std::string Path = Impl->Options.OutputPath.empty() ? BaseName + ".o" : Impl->Options.OutputPath;
        if ( not Impl->EmitObjectFile( Path ) )
        {
            return MakeFailure();
        }
        return EmitResult{ .Status = EEmitStatus::Ok, .Artifact = Path, .Message = {} };
    }

    // Link: the object file is a temporary intermediate — nothing downstream
    // of the linker ever reads it — removed once the link either succeeds or
    // fails.
    llvm::SmallString<128> TempObject;
    if ( const std::error_code Error = llvm::sys::fs::createTemporaryFile( BaseName, "o", TempObject ) )
    {
        static_cast<void>( Impl->Fail( "llvm: could not create a temporary object file: " + Error.message() ) );
        return MakeFailure();
    }
    const llvm::FileRemover ObjectCleanup( TempObject.str() );

    if ( not Impl->EmitObjectFile( TempObject.str() ) )
    {
        return MakeFailure();
    }

    const std::string OutputPath = Impl->Options.OutputPath.empty() ? std::string( "a.out" ) : Impl->Options.OutputPath;
    if ( not Impl->LinkExecutable( TempObject.str(), OutputPath ) )
    {
        return MakeFailure();
    }

    return EmitResult{ .Status = EEmitStatus::Ok, .Artifact = OutputPath, .Message = {} };
}
