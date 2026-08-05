#pragma once

// PchLLVM.hpp — the precompiled header every BackendLLVM TU is built against.
//
// The module compiles with `unity=off` (meson.build): ~90 small translation
// units, each of which would otherwise re-parse the same tens of thousands of
// lines of LLVM template code. Everything listed here is (a) from LLVM,
// (b) stable — it changes only when the LLVM dependency does — and (c) used by
// more than one TU. Project headers are deliberately absent: they are what
// actually churn, and a PCH containing them would be rebuilt on every edit,
// which is the opposite of the point.

// --- LLVM IR core ----------------------------------------------------------
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>

// --- Passes, target and support --------------------------------------------
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FileUtilities.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
