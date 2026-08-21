// ExceptionGlobals.cpp — the three slots the transport uses, by either route.
//
// Thread-local because the transport is per-thread state, not a value carried by
// any signature: abi.md reserves no channel for it, which is exactly what makes
// tier 1 cost the calling convention nothing.
//
// Two routes to the same three slots, chosen by IrOptions::Tls and differing
// nowhere else in the module:
//
//   Direct    — the slots *are* three thread-local globals, created here once
//               and addressed directly. What `volt build` emits, and what a
//               static linker resolves for free.
//   Accessor  — the slots are wherever `__volt_unwind_slots` says they are, and
//               this module only calls it. What the JIT emits, because JIT-ed
//               code cannot carry TLS relocations without an ORC runtime whose
//               version is not ours to pin (UnwindTransport.hpp states why).
//
// The consequence worth naming: an Accessor address is the result of a call, so
// it belongs to the block it was emitted into and is *not* cached. A Direct
// address is a module-level global and is. That asymmetry is the entire reason
// these functions take a builder.

#include "Lower/Exception/ExceptionLowering.hpp"

#include "Core/ModuleContext.hpp"
#include "Volt/BackendCore/UnwindTransport.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <string>
#include <vector>

llvm::Value *
Volt::Backend::Llvm::ExceptionLowering::SlotThroughAccessor ( llvm::IRBuilderBase &Into, std::size_t Index, const char *Name )
{
    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::Type *Address        = llvm::PointerType::get( Context, 0 );

    // A table of addresses, not a block of storage: the definition owns the
    // slots and hands back where they live, so JIT-ed code and a precompiled
    // stdlib share one copy of the transport state. UnwindTransport.hpp is
    // where that contract is stated and why.
    const std::vector<llvm::Type *> Fields( Volt::Backend::UnwindTransport::SlotTableEntries, Address );
    llvm::StructType *TableTy = llvm::StructType::get( Context, Fields, false );

    // getOrInsertFunction, so the declaration appears once however many bodies
    // reach a slot. Nothing defines it here — the JIT resolves it against the
    // stdlib shared object, or against the compiler's own symbols.
    const llvm::FunctionCallee Accessor = Services->Ctx->Mod().getOrInsertFunction(
        std::string( Volt::Backend::UnwindTransport::SlotAccessorSymbol ), llvm::FunctionType::get( Address, false ) );

    llvm::Value *Table = Into.CreateCall( Accessor, {}, "unwind.slots" );
    llvm::Value *Entry = Into.CreateStructGEP( TableTy, Table, static_cast<unsigned>( Index ), "unwind.slot.addr" );
    return Into.CreateLoad( Address, Entry, Name );
}

llvm::Value *Volt::Backend::Llvm::ExceptionLowering::ExceptionValueSlot ( llvm::IRBuilderBase &Into )
{
    if ( Services->Options->Tls == Ir::ETlsAccess::Accessor )
    {
        return SlotThroughAccessor( Into, Volt::Backend::UnwindTransport::SlotTableValueIndex, "exc.value.slot" );
    }

    if ( ExcValue != nullptr )
    {
        return ExcValue;
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::Type *Address        = llvm::PointerType::get( Context, 0 );
    ExcValue = new llvm::GlobalVariable( Services->Ctx->Mod(), Address, false, llvm::GlobalValue::LinkOnceODRLinkage,
                                         llvm::Constant::getNullValue( Address ),
                                         std::string( Volt::Backend::UnwindTransport::ExceptionValueSlot ) );
    ExcValue->setThreadLocal( true );
    return ExcValue;
}

llvm::Value *Volt::Backend::Llvm::ExceptionLowering::ExceptionTagSlot ( llvm::IRBuilderBase &Into )
{
    if ( Services->Options->Tls == Ir::ETlsAccess::Accessor )
    {
        return SlotThroughAccessor( Into, Volt::Backend::UnwindTransport::SlotTableTagIndex, "exc.tag.slot" );
    }

    if ( ExcTag != nullptr )
    {
        return ExcTag;
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::Type *Int32Ty        = llvm::Type::getInt32Ty( Context );
    ExcTag = new llvm::GlobalVariable( Services->Ctx->Mod(), Int32Ty, false, llvm::GlobalValue::LinkOnceODRLinkage,
                                       llvm::ConstantInt::get( Int32Ty, MiddleEnd::TypeSystem::NominalId::InvalidValue ),
                                       std::string( Volt::Backend::UnwindTransport::ExceptionTagSlot ) );
    ExcTag->setThreadLocal( true );
    return ExcTag;
}

llvm::Value *Volt::Backend::Llvm::ExceptionLowering::BreakFlagSlot ( llvm::IRBuilderBase &Into )
{
    if ( Services->Options->Tls == Ir::ETlsAccess::Accessor )
    {
        return SlotThroughAccessor( Into, Volt::Backend::UnwindTransport::SlotTableBreakIndex, "brk.flag.slot" );
    }

    if ( BrkFlag != nullptr )
    {
        return BrkFlag;
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::Type *BoolTy         = llvm::Type::getInt1Ty( Context );
    BrkFlag = new llvm::GlobalVariable( Services->Ctx->Mod(), BoolTy, false, llvm::GlobalValue::LinkOnceODRLinkage,
                                        llvm::ConstantInt::getFalse( BoolTy ),
                                        std::string( Volt::Backend::UnwindTransport::BreakFlagSlot ) );
    BrkFlag->setThreadLocal( true );
    return BrkFlag;
}

bool Volt::Backend::Llvm::ExceptionLowering::EmitSlotAccessor ()
{
    // Nothing to point at: under Accessor this module *calls* the accessor
    // rather than owning the slots, so defining one here would be circular.
    if ( Services->Options->Tls == Ir::ETlsAccess::Accessor )
    {
        return true;
    }

    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::Module &Mod          = Services->Ctx->Mod();
    llvm::Type *Address        = llvm::PointerType::get( Context, 0 );

    const std::string Name( Volt::Backend::UnwindTransport::SlotAccessorSymbol );
    if ( Mod.getFunction( Name ) != nullptr )
    {
        return true;
    }

    const std::vector<llvm::Type *> Fields( Volt::Backend::UnwindTransport::SlotTableEntries, Address );
    llvm::StructType *TableTy = llvm::StructType::get( Context, Fields, false );

    // The table is itself thread-local: it holds this thread's slot addresses,
    // and a TLS address is not a constant, so it cannot be a static
    // initialiser. Filling it is the accessor's body.
    auto *Table = new llvm::GlobalVariable( Mod, TableTy, false, llvm::GlobalValue::InternalLinkage,
                                            llvm::Constant::getNullValue( TableTy ), "volt.exc.slots" );
    Table->setThreadLocal( true );

    llvm::Function *Fn =
        llvm::Function::Create( llvm::FunctionType::get( Address, false ), llvm::Function::ExternalLinkage, Name, &Mod );
    llvm::IRBuilder<> Shell{ llvm::BasicBlock::Create( Context, "entry", Fn ) };

    // Stored unconditionally on every call rather than behind a
    // once-initialised guard: four stores of a constant address are cheaper
    // than the branch and the flag would be, and there is nothing to get wrong.
    const std::pair<std::size_t, llvm::Value *> Slots[] = {
        { Volt::Backend::UnwindTransport::SlotTableValueIndex, ExceptionValueSlot( Shell ) },
        { Volt::Backend::UnwindTransport::SlotTableTagIndex, ExceptionTagSlot( Shell ) },
        { Volt::Backend::UnwindTransport::SlotTableBreakIndex, BreakFlagSlot( Shell ) },
        { Volt::Backend::UnwindTransport::SlotTableStorageIndex, ExceptionStorageSlot( Shell ) },
    };

    for ( const auto &[Index, Slot] : Slots )
    {
        llvm::Value *Entry = Shell.CreateStructGEP( TableTy, Table, static_cast<unsigned>( Index ) );
        static_cast<void>( Shell.CreateStore( Slot, Entry ) );
    }

    static_cast<void>( Shell.CreateRet( Table ) );
    return true;
}
