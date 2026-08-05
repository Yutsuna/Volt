#pragma once

// LlvmFwd.hpp — forward declarations of the LLVM types this module's private
// headers mention.
//
// The point is inclusion discipline: a header that only *names* `llvm::Value *`
// in a signature needs no LLVM header at all, and pulling one in would put tens
// of thousands of lines of template code behind every include of it. The rule
// this file exists to enforce (see the module's PchLLVM.hpp and meson.build):
//
//   - private headers include this and declare;
//   - `.cpp` files include the real LLVM headers they use;
//   - the public header (Public/Volt/BackendLLVM/LlvmEmitter.hpp) mentions no
//     LLVM type whatsoever, forward-declared or not.
//
// Nothing here may become an `#include <llvm/...>`. The one deliberate
// exception in this module is ModuleContext.hpp, which owns an
// `llvm::IRBuilder<>` by value and therefore cannot forward-declare it.

namespace llvm
{

class AllocaInst;
class Argument;
class BasicBlock;
class Constant;
class ConstantInt;
class DataLayout;
class Function;
class FunctionType;
class GlobalVariable;
class LLVMContext;
class Module;
class PHINode;
class StructType;
class TargetMachine;
class Type;
class Value;
class raw_ostream;

} // namespace llvm
