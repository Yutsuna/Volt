// source/Volt/Backend/BackendLLVM/Private/LlvmEmitter.cpp

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
    // A nested `self` (`Comparable#..`'s `-> Range<self>`, not a bare `->
    // self`) needs the receiver's own MonoRequest encoding to substitute
    // into — the same one `Owner`/`FlatArgs` already describe.
    return Instances.OfSignature( Store, Id, FlatArgs, Backend::SelfSubtree( Store, Owner, FlatArgs ) );
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

    // A monomorphised instantiation (FlatArgs non-empty — only
    // EmitMonomorphizedBody ever calls FunctionFor that way; every concrete
    // declare/define call site passes {}) is `linkonce_odr`, not
    // `External`: `Array<UInt8>` used internally by a precompiled stdlib
    // archive and independently instantiated by a user build mangle to the
    // *same* symbol (Mangler.hpp is deterministic and content-only), and
    // without weak linkage the two definitions collide at link time
    // (issue #61 blind-spot #3). `linkonce_odr` tells the linker any one
    // definition will do, which is exactly true — Monomorphizer only ever
    // reinstantiates the same body for the same FlatArgs.
    const llvm::GlobalValue::LinkageTypes Linkage =
        FlatArgs.empty() ? llvm::Function::ExternalLinkage : llvm::Function::LinkOnceODRLinkage;
    llvm::Function *Fn = llvm::Function::Create( Signature, Linkage, Symbol, Mod.get() );
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
    if ( Build == nullptr or Build->Types == nullptr )
    {
        return;
    }

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

    std::size_t Ordinal = 0;
    for ( const Frontend::ParamId ParamRef : Node->Params )
    {
        llvm::Value *Arg                = Fn->getArg( Index++ );
        const Frontend::Param &Declared = Unit.Ast->GetParam( ParamRef );
        Arg->setName( Unit.Ast->Text( Declared.Name ) );

        // Read out of the *signature*'s own inputs, in the order FunctionTypeOf
        // consumed them, so "by address" cannot drift between the two.
        const bool bBlock     = Ordinal < Entry.ParamIsBlock.Size() and Entry.ParamIsBlock[Ordinal];
        const bool bByAddress = bBlock or ( Ordinal < Entry.Params.Size() and
                                            IsAggregate( SignatureLayoutOf( Store, Entry.Params[Ordinal], Owner, {} ) ) );
        ++Ordinal;

        if ( not BindParameter( Sema::BindingSite{ ParamRef }, Arg, bByAddress, Unit.Ast->Text( Declared.Name ) ) )
        {
            static_cast<void>( Fail( "llvm: parameter '" + std::string( Unit.Ast->Text( Declared.Name ) ) + "' of '" +
                                     std::string( Store.Text( Entry.Name ) ) + "' has no storage" ) );
            return;
        }

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
            // `getNullValue`, not `ConstantInt::get`: the return type is not
            // always an integer (a `String`-returning method is `{ ptr, i64 }`),
            // and `ConstantInt::get` on a non-integer type is an unchecked
            // `cast` in a release build — silent corruption, not a diagnostic.
            static_cast<void>( Builder->CreateRet( llvm::Constant::getNullValue( Fn->getReturnType() ) ) );
        }
        else
        {
            static_cast<void>( Builder->CreateRetVoid() );
        }
    }

    Frame = FunctionFrame{};
}

void Volt::Backend::Llvm::LlvmBackend::State::DeclareSynthesized ( const UnitView &Unit )
{
    if ( Unit.Synth == nullptr )
    {
        return;
    }
    for ( const Sema::SynthesizedFunction &Fn : Unit.Synth->All() )
    {
        // No self, no mangling, no cross-unit symbol: this function is
        // reached only through a FuncAddr naming Fn.Decl directly, so its
        // LLVM name only has to be distinct within the module, never
        // resolvable.
        std::vector<llvm::Type *> Params;
        Params.reserve( Fn.Params.Size() );
        bool bOk = true;
        for ( const Sema::SemaTypeId Param : Fn.Params )
        {
            llvm::Type *Slot = ParamTypeOfLayout( LayoutOfValue( *Unit.Values, Param ) );
            if ( Slot == nullptr )
            {
                static_cast<void>( Fail( "llvm: a synthesized function's parameter in " + std::string( Unit.Path ) +
                                         " has no resolved layout" ) );
                bOk = false;
                break;
            }
            Params.push_back( Slot );
        }
        if ( not bOk )
        {
            continue;
        }

        llvm::Type *Result = Fn.Result.IsValid() ? TypeOfLayout( LayoutOfValue( *Unit.Values, Fn.Result ) ) : nullptr;
        if ( Result == nullptr )
        {
            Result = llvm::Type::getVoidTy( Context );
        }

        llvm::FunctionType *Signature = llvm::FunctionType::get( Result, Params, false );
        const std::string Name        = "__synth." + std::to_string( Unit.Ordinal ) + "." + std::to_string( Fn.Decl.Value );
        llvm::Function *LlvmFn        = llvm::Function::Create( Signature, llvm::Function::PrivateLinkage, Name, Mod.get() );
        SynthesizedFns.emplace( UnitDeclKey{ .Ordinal = Unit.Ordinal, .Decl = Fn.Decl }, LlvmFn );
    }
}

void Volt::Backend::Llvm::LlvmBackend::State::DefineSynthesized ( const UnitView &Unit )
{
    if ( Unit.Synth == nullptr )
    {
        return;
    }
    for ( const Sema::SynthesizedFunction &Fn : Unit.Synth->All() )
    {
        DefineSynthesizedFn( Fn, Unit );
    }
}

void Volt::Backend::Llvm::LlvmBackend::State::DefineSynthesizedFn ( const Sema::SynthesizedFunction &Fn, const UnitView &Unit )
{
    const auto *Node = std::get_if<Frontend::Method>( &Unit.Ast->Decl( Fn.Decl ) );
    if ( Node == nullptr )
    {
        static_cast<void>(
            Fail( "llvm: a synthesized function in " + std::string( Unit.Path ) + " has no Method declaration behind it" ) );
        return;
    }

    const auto It = SynthesizedFns.find( UnitDeclKey{ .Ordinal = Unit.Ordinal, .Decl = Fn.Decl } );
    if ( It == SynthesizedFns.end() )
    {
        // DeclareSynthesized already reported why this entry has no
        // llvm::Function.
        return;
    }
    llvm::Function *LlvmFn = It->second;
    llvm::Type *Result     = LlvmFn->getReturnType();

    Frame               = FunctionFrame{};
    Frame.Fn            = LlvmFn;
    Frame.Unit          = &Unit;
    Frame.Values        = Unit.Values;
    Frame.Callees       = Unit.Callees;
    Frame.Entry         = llvm::BasicBlock::Create( Context, "entry", LlvmFn );
    Frame.bReturnsValue = not Result->isVoidTy();
    // Every SynthesizedFunction is a lifted Lambda/Block (ClosureLifting is
    // its only producer), so a bare `break`/`next` in its body is the same
    // non-local exit ClosureEmitter's EmitClosureBody used to translate
    // directly — bClosure routes StmtEmitter's Break/Next arms onto the
    // unwind-sentinel transport (BreakFlagSlot/EmitBlockNext) instead of
    // failing with "outside a loop reached codegen".
    Frame.bClosure = true;
    Builder->SetInsertPoint( Frame.Entry );

    unsigned Index = 0;
    for ( const Frontend::ParamId ParamRef : Node->Params )
    {
        llvm::Value *Arg                = LlvmFn->getArg( Index++ );
        const Frontend::Param &Declared = Unit.Ast->GetParam( ParamRef );
        Arg->setName( Unit.Ast->Text( Declared.Name ) );

        const Sema::BindingSite Site{ ParamRef };
        const bool bByAddress = IsAggregate( LayoutOfValue( *Unit.Values, Unit.Values->SiteType( Site ) ) );
        if ( not BindParameter( Site, Arg, bByAddress, Unit.Ast->Text( Declared.Name ) ) )
        {
            static_cast<void>( Fail( "llvm: a synthesized function's parameter '" +
                                     std::string( Unit.Ast->Text( Declared.Name ) ) + "' has no storage" ) );
            return;
        }
    }

    EmitStmts( Node->Body, Frame.bReturnsValue );

    if ( not Terminated() )
    {
        if ( Frame.bReturnsValue )
        {
            static_cast<void>( Builder->CreateRet( llvm::Constant::getNullValue( Result ) ) );
        }
        else
        {
            static_cast<void>( Builder->CreateRetVoid() );
        }
    }

    Frame = FunctionFrame{};
}

bool Volt::Backend::Llvm::LlvmBackend::State::BindParameter ( const Sema::BindingSite &Site,
                                                              llvm::Value *Arg,
                                                              bool bByAddress,
                                                              std::string_view Name )
{
    // An aggregate (and a `&block`, whose `{ code, env }` pair is one) arrives
    // as a pointer to the caller's storage and *is* its own slot, so it is kept
    // as-is. A scalar arrives as a bare value with no backing storage — without
    // an alloca here, a later read of it as an Identifier (LoadPlace ->
    // EmitAddress -> CreateLoad) loads *through* the value as if it pointed at
    // itself.
    //
    // The question is the parameter's **layout**, never its LLVM type. `ptr` is
    // not a proxy for "aggregate": a `@[Primitive( "ptr", 64 )]` scalar — every
    // `Pointer<T>` — maps to `ptr` too, and answering from the LLVM type made
    // every pointer parameter its own slot, so `String.from_c_string( p )` read
    // `*p` instead of `p` and handed `strlen` whatever the pointee held.
    if ( bByAddress )
    {
        Frame.Slots.emplace( Site, Arg );
        return true;
    }

    llvm::Value *Slot = SlotFor( Site, Arg->getType(), Name );
    if ( Slot == nullptr )
    {
        return false;
    }
    static_cast<void>( Builder->CreateStore( Arg, Slot ) );
    return true;
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

bool Volt::Backend::Llvm::LlvmBackend::State::EmitInitAll ()
{
    // `_V_init_all`: the one symbol the stdlib prelude's
    // `@[External( "volt", "_V_init_all" )]` declaration names. DeclareAll
    // has already created it as an external declaration — the same shape
    // every other @[External] member gets, "declared, never defined" — this
    // gives that declaration its one and only body, called exactly once, by
    // the prelude's `__volt_entry`. A raise inside it is carried out through
    // the ordinary post-call check every Volt call site already gets
    // (EmitExceptionCheck, wired into EmitResolvedCall); this function's own
    // job is only to stop running *further* unit inits once one has left the
    // tag set, hand-rolled because it is synthesised, not emitted from a
    // Volt body.
    llvm::Function *InitAllFn = Mod->getFunction( "_V_init_all" );
    if ( InitAllFn == nullptr )
    {
        llvm::FunctionType *FnTy = llvm::FunctionType::get( Builder->getVoidTy(), false );
        InitAllFn                = llvm::Function::Create( FnTy, llvm::Function::ExternalLinkage, "_V_init_all", Mod.get() );
    }
    if ( not InitAllFn->empty() )
    {
        return true;
    }

    llvm::Type *Int32Ty = llvm::Type::getInt32Ty( Context );
    llvm::IRBuilder<> Shell{ llvm::BasicBlock::Create( Context, "entry", InitAllFn ) };

    if ( Build != nullptr )
    {
        for ( std::size_t Index = 0; Index < Build->Units.size(); ++Index )
        {
            const UnitView &Unit       = Build->Units[Index];
            const std::string InitName = "_V_init_" + std::to_string( Unit.Ordinal );
            llvm::Function *InitFn     = Mod->getFunction( InitName );
            if ( InitFn == nullptr )
            {
                llvm::FunctionType *FnTy = llvm::FunctionType::get( Builder->getVoidTy(), false );
                InitFn                   = llvm::Function::Create( FnTy, llvm::Function::ExternalLinkage, InitName, Mod.get() );
            }
            static_cast<void>( Shell.CreateCall( InitFn ) );

            if ( Index + 1 < Build->Units.size() )
            {
                llvm::Value *Tag = Shell.CreateLoad( Int32Ty, ExceptionTagSlot(), "exc.tag" );
                llvm::Value *Pending =
                    Shell.CreateICmpNE( Tag, llvm::ConstantInt::get( Int32Ty, Sema::NominalId::InvalidValue ), "exc.pending" );

                llvm::BasicBlock *Stop = llvm::BasicBlock::Create( Context, "init.stop", InitAllFn );
                llvm::BasicBlock *Next = llvm::BasicBlock::Create( Context, "init.next", InitAllFn );
                static_cast<void>( Shell.CreateCondBr( Pending, Stop, Next ) );

                Shell.SetInsertPoint( Stop );
                static_cast<void>( Shell.CreateRetVoid() );

                Shell.SetInsertPoint( Next );
            }
        }
    }

    static_cast<void>( Shell.CreateRetVoid() );
    return true;
}

bool Volt::Backend::Llvm::LlvmBackend::State::EmitEntryPoint ()
{
    // The C entry symbol (e.g. `main`). If no entry symbol is requested or if
    // one is already defined in the LLVM module, skip emitting the entry point.
    if ( Options.EntrySymbol.empty() or Mod->getFunction( Options.EntrySymbol ) != nullptr )
    {
        return true;
    }

    if ( not EmitInitAll() )
    {
        return false;
    }

    // The Volt free function the C runtime hands control to
    // (source/Lib/Prelude.vl's `__volt_entry`, by default). DeclareAll has
    // already emitted its `llvm::Function` by the time Finalize reaches this
    // seam, exactly like any other free function — reporting an uncaught
    // exception and choosing the exit status are that function's own
    // `begin/rescue`, not this file's business (rules/zero-hardcode.md): no
    // field name, no type name, no byte of message enters C++ here.
    llvm::Function *EntryFn = nullptr;
    if ( Build != nullptr and Build->Types != nullptr )
    {
        if ( const Sema::Member *Entry = Build->Types->LookupFunction( Options.EntryFunction ); Entry != nullptr )
        {
            EntryFn = FunctionFor( *Entry, Sema::NominalId{}, {} );
        }
    }
    if ( EntryFn == nullptr )
    {
        static_cast<void>(
            Fail( "llvm: entry function '" + Options.EntryFunction + "' not found — the stdlib prelude must declare it" ) );
        return false;
    }

    llvm::Type *ExitCodeTy = llvm::Type::getInt32Ty( Context );
    llvm::Type *ArgcTy     = llvm::Type::getInt32Ty( Context );
    llvm::Type *ArgvTy     = llvm::PointerType::getUnqual( Context );

    llvm::FunctionType *MainTy = llvm::FunctionType::get( ExitCodeTy, { ArgcTy, ArgvTy }, false );
    llvm::Function *MainFn = llvm::Function::Create( MainTy, llvm::Function::ExternalLinkage, Options.EntrySymbol, Mod.get() );

    llvm::IRBuilder<> Shell{ llvm::BasicBlock::Create( Context, "entry", MainFn ) };
    llvm::Value *ExitCode = Shell.CreateCall( EntryFn, {}, "exit.code" );
    static_cast<void>( Shell.CreateRet( ExitCode ) );
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
    if ( Build == nullptr or Build->Types == nullptr )
    {
        return;
    }

    Sema::TypeStore &Store = *Build->Types;

    // Declared first, for the same reason DeclareAll runs before any body:
    // a FuncAddr inside an ordinary member's own body (an original closure
    // literal's call site) must find its target already registered.
    DeclareSynthesized( Unit );

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

    DefineSynthesized( Unit );

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

    if ( Impl->Build == nullptr or Impl->Build->Types == nullptr )
    {
        return Impl->Fail( "llvm: unit '" + std::string( Unit.Path ) +
                           "' reached the backend with no build input or type store" );
    }

    if ( Unit.Ast == nullptr or Unit.Values == nullptr or Unit.Callees == nullptr or Unit.Scopes == nullptr )
    {
        return Impl->Fail( "llvm: unit '" + std::string( Unit.Path ) + "' reached the backend with no sema output" );
    }

    // A stdlib unit under a precompiled build never gets a body here: it is
    // already declared (DeclareAll runs over the whole TypeStore regardless
    // of unit) and its definition is expected from the linked archive/.so
    // instead — the same "declared, never defined" shape @[External]
    // members already have.
    if ( Impl->Options.bStdlibPrecompiled and Unit.Ordinal < Impl->Build->StdlibUnitCount )
    {
        return EEmitStatus::Ok;
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

    const std::string DefaultName = Impl->Options.bSharedOutput ? BaseName + ".so" : std::string( "a.out" );
    const std::string OutputPath  = Impl->Options.OutputPath.empty() ? DefaultName : Impl->Options.OutputPath;

    const bool bLinked = Impl->Options.bSharedOutput ? Impl->LinkSharedLibrary( TempObject.str(), OutputPath )
                                                     : Impl->LinkExecutable( TempObject.str(), OutputPath );
    if ( not bLinked )
    {
        return MakeFailure();
    }

    return EmitResult{ .Status = EEmitStatus::Ok, .Artifact = OutputPath, .Message = {} };
}
