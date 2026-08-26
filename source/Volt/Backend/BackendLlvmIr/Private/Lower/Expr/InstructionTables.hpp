#pragma once

// InstructionTables.hpp — the LLVM encoding of a neutral machine opcode.
//
// Which family × operator selects which opcode is stated once, upstream, in
// BackendCore/Instructions.inl, and read through BackendCore's FindBinOp /
// FindCmp / FindUnOp. What is left for this module is one column: the map from
// EBinOp / ECmpPred to the LLVM enumerators, written in LlvmOpcodes.inl and
// expanded here. No row is repeated, and no emitter in this module switches on
// an operator (rules/meta-first.md).

#include "Volt/BackendCore/InstructionSchema.hpp"

#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        [[nodiscard]] llvm::Instruction::BinaryOps EncodingOf ( EBinOp Op );
        [[nodiscard]] llvm::CmpInst::Predicate EncodingOf ( ECmpPred Predicate );

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
