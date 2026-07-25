// LlvmEmitter.cpp — LlvmBackend's lifecycle and the two sweeps over the units.
//
// The emission itself lives in sibling TUs (TypeMapper, ExprEmitter, ...);
// this file owns only the shape of a build: set up the module and the host
// target, declare everything reachable, then define it.

#include "LlvmState.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/MC/TargetRegistry.h>
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
        Status  = EEmitStatus::Error;
        Message = std::move( InMessage );
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

    Machine.reset( Target->createTargetMachine( Triple, "generic", "", llvm::TargetOptions{}, std::nullopt ) );
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

        llvm::Type *Slot = ParamTypeOfLayout( Instances.OfSignature( Store, Entry.Params[Index], FlatArgs ) );
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
        Entry.Result.IsValid() ? TypeOfLayout( Instances.OfSignature( Store, Entry.Result, FlatArgs ) ) : nullptr;
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
        if ( Store.Type( Id ).Params.Size() > 0 )
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
        if ( Index >= Fn->arg_size() )
        {
            static_cast<void>( Fail( "llvm: '" + std::string( Store.Text( Entry.Name ) ) +
                                     "' declares more parameters in its AST than in its signature" ) );
            return;
        }

        const Frontend::Param &Declared = Unit.Ast->GetParam( ParamRef );
        llvm::Argument *Arg             = Fn->getArg( Index );
        Arg->setName( Unit.Ast->Text( Declared.Name ) );
        ++Index;

        // An aggregate already arrives as a pointer to storage, which *is* its
        // slot; only a scalar needs an alloca, and it needs one so that
        // assigning to a parameter works and mem2reg can undo it.
        const Sema::BindingSite Site{ ParamRef };
        if ( Arg->getType()->isPointerTy() )
        {
            Frame.Slots.emplace( Site, Arg );
            continue;
        }

        llvm::Value *Slot = SlotFor( Site, Arg->getType(), Unit.Ast->Text( Declared.Name ) );
        if ( Slot == nullptr )
        {
            static_cast<void>( Fail( "llvm: parameter '" + std::string( Unit.Ast->Text( Declared.Name ) ) + "' of '" +
                                     std::string( Store.Text( Entry.Name ) ) + "' has no storage" ) );
            return;
        }
        static_cast<void>( Builder->CreateStore( Arg, Slot ) );
    }

    EmitStmts( Node->Body, Frame.bReturnsValue );

    if ( not Terminated() )
    {
        if ( Frame.bReturnsValue )
        {
            // Falling off the end of a value-returning body. Volt has no
            // definite-return analysis (an `if` with no `else` in tail position
            // is accepted), so this path is reachable from valid source and
            // cannot be a hard failure — `unreachable` is the honest lowering:
            // it says "the middle-end promised control never gets here". Listed
            // as a middle-end gap in .agents/backend/llvm.md.
            static_cast<void>( Builder->CreateUnreachable() );
        }
        else
        {
            static_cast<void>( Builder->CreateRetVoid() );
        }
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
        if ( Store.Type( Id ).Params.Size() > 0 )
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
}

Volt::Backend::Llvm::LlvmBackend::LlvmBackend () : Impl( std::make_unique<State>() )
{
}

Volt::Backend::Llvm::LlvmBackend::~LlvmBackend () = default;

Volt::Backend::Llvm::LlvmBackend::LlvmBackend ( LlvmBackend && ) noexcept = default;

Volt::Backend::Llvm::LlvmBackend &Volt::Backend::Llvm::LlvmBackend::operator=( LlvmBackend && ) noexcept = default;

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
    if ( Impl->Failed() )
    {
        return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = Impl->Message };
    }

    // Every unit is defined by now, so every instantiation a concrete body
    // could ever discover has been enqueued. A drained body can itself
    // enqueue more — a generic method calling another generic method — so
    // this drains to a fixpoint rather than once.
    Impl->DrainMonomorphizer();
    if ( Impl->Failed() )
    {
        return EmitResult{ .Status = EEmitStatus::Error, .Artifact = {}, .Message = Impl->Message };
    }

    return EmitResult{
        .Status = EEmitStatus::Unimplemented, .Artifact = {}, .Message = "llvm backend: emission not implemented yet" };
}
