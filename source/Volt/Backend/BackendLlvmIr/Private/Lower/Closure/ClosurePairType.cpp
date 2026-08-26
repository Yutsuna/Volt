// ClosurePairType.cpp — the closure value's shape.

#include "Lower/Closure/ClosureLowering.hpp"

#include "Core/ModuleContext.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

llvm::StructType *Volt::Backend::Llvm::ClosureLowering::ClosurePairType ()
{
    llvm::LLVMContext &Context = Services->Ctx->Context();
    llvm::Type *Address        = llvm::PointerType::get( Context, 0 );
    // A literal struct type, uniqued by LLVM on its element types — so this is
    // the same `{ ptr, ptr }` TypeMapper builds from the layout InstanceLayouts
    // materialises for a callable, not a second shape that happens to match.
    return llvm::StructType::get( Context, { Address, Address }, false );
}
