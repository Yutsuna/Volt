#pragma once

// FunctionSlots.hpp — `@volt.fn.<symbol>`, the seam a hot reload repoints.
//
// Under ELinkage::Direct a call is `call @sym`: the callee's address is a
// relocation, fixed the moment the code is linked or materialised, and the only
// way to change it afterwards is to recompile every caller. Under Indirect the
// same call becomes a load of `@volt.fn.sym` followed by a call through the
// loaded pointer, so repointing a function is one aligned pointer store and the
// callers never learn about it.
//
// The slot is *defined* in whichever module defines the body — an intra-module
// relocation, so no laziness is lost — and declared external everywhere else.
// Nothing outside this file needs to know the name it is spelled with.
//
// Not every callee gets one. A symbol this build does not define cannot be
// reloaded by this build, so a call into the precompiled stdlib or across an
// @[External] boundary stays direct and pays nothing: FunctionRegistry decides
// which symbols are indirectable when it declares them, and answers here.

#include "Core/LlvmFwd.hpp"
#include "Volt/BackendLlvmIr/IrGenerator.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace llvm
{
class Value;
}

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        struct EmitterServices;

        // The slot for `Fn`, as seen from the module being written; created as
        // an external declaration when this module has not seen it yet. Null
        // when the options ask for direct linkage or `Fn` is not indirectable,
        // which is the caller's signal to use `Fn` itself.
        [[nodiscard]] llvm::GlobalVariable *SlotFor ( EmitterServices &Services, llvm::Function *Fn );

        // What a call should call: `Fn` under direct linkage, and a load of its
        // slot under indirect. Emitted at the builder's current insert point.
        [[nodiscard]] llvm::Value *CalleeValue ( EmitterServices &Services, llvm::Function *Fn );

        // Give every indirectable body the module being written defines its
        // slot, initialised to that body. Runs once per module, at the point
        // the module is closed — by then every body it will ever hold is in it,
        // so no call site has to care whether the callee was emitted before or
        // after itself.
        void DefineLocalSlots ( EmitterServices &Services );

        // The symbols the module being written defines and that a reload would
        // have to repoint, each with its lowered signature. Read at the end of
        // each EmitUnit.
        [[nodiscard]] std::vector<Ir::IrGenerator::UnitSymbol> LocalDefinedSymbols ( EmitterServices &Services );

        // Every name the current module puts in the symbol table — functions
        // with a body and globals with an initialiser — with no filter at all.
        // What a consumer emitting repeatedly into one live program needs, and
        // strictly wider than LocalDefinedSymbols: a monomorphisation is
        // defined here without being indirectable.
        [[nodiscard]] std::vector<std::string> LocalDefinedNames ( EmitterServices &Services );

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
