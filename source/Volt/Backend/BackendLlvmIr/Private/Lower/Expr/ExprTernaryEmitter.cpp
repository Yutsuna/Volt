// ExprTernaryEmitter.cpp — `c ? a : b`.
//
// The one slot-free convergence in the expression category: both arms are single
// *expressions*, so the block count reaching the merge is fixed at two and a phi
// is exactly right — unlike `if`/`case`/`begin`, whose arms are statement lists.
// Two blocks and a phi, never a `select`: `select` would evaluate the arm not
// taken.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

// NOLINTBEGIN(clang-analyzer-security.ArrayBound) — false positive via LLVM's hung-off operand layout
llvm::Value *Volt::Backend::Llvm::EmitTernary ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::Ternary &Node )
{
    llvm::Value *Cond = Emitter.EmitExpr( Node.Cond );
    if ( Cond == nullptr )
    {
        return nullptr;
    }

    llvm::LLVMContext &Context = Emitter.Ctx().Context();
    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();
    llvm::Function *Fn         = Emitter.Frame().Fn;

    llvm::BasicBlock *ThenBlock = llvm::BasicBlock::Create( Context, "ternary.then", Fn );
    llvm::BasicBlock *ElseBlock = llvm::BasicBlock::Create( Context, "ternary.else", Fn );
    llvm::BasicBlock *Merge     = llvm::BasicBlock::Create( Context, "ternary.end", Fn );
    static_cast<void>( Builder.CreateCondBr( Cond, ThenBlock, ElseBlock ) );

    Builder.SetInsertPoint( ThenBlock );
    llvm::Value *Then = Emitter.EmitExpr( Node.Then );
    if ( Then == nullptr )
    {
        return nullptr;
    }
    llvm::BasicBlock *ThenEnd = Builder.GetInsertBlock();
    static_cast<void>( Builder.CreateBr( Merge ) );

    Builder.SetInsertPoint( ElseBlock );
    llvm::Value *Else = Emitter.EmitExpr( Node.Else );
    if ( Else == nullptr )
    {
        return nullptr;
    }
    llvm::BasicBlock *ElseEnd = Builder.GetInsertBlock();
    static_cast<void>( Builder.CreateBr( Merge ) );

    Builder.SetInsertPoint( Merge );
    // An aggregate arm evaluates to a `ptr` at its storage, never to the struct
    // value itself (the ABI convention this whole module follows) — so the PHI
    // must merge on `ptr`, exactly what ParamTypeOfLayout already computes for a
    // by-pointer parameter. Using TypeOfExpr's struct type here instead crashes
    // CreatePHI/addIncoming the moment either arm is a `String`/user-struct value
    // (`self ? "true" : "false"`), since Then/Else are pointers but the PHI's
    // declared type would not be.
    llvm::Type *Shape = Emitter.Types().ParamTypeOfLayout( Emitter.LayoutOfExpr( Id ) );
    if ( Shape == nullptr )
    {
        Shape = Then->getType();
    }
    // CoerceWidth reconciles two integer widths and nothing else, so an arm that
    // is a different *kind* of value (an integer against a pointer, a float
    // against an integer) survives it unchanged. Merging those into one PHI is an
    // assertion failure inside LLVM, i.e. a crash on well-typed Volt — which is
    // the one outcome this backend must never produce. Refuse by name instead: it
    // means the two arms were given types that do not meet, and that is a
    // middle-end answer, not something to repair here.
    llvm::Value *ThenValue = Emitter.CoerceWidth( Then, Shape );
    llvm::Value *ElseValue = Emitter.CoerceWidth( Else, Shape );
    if ( ThenValue->getType() != Shape or ElseValue->getType() != Shape )
    {
        std::string Report;
        llvm::raw_string_ostream Out{ Report };
        Out << "llvm: the arms of the ternary at expression " << Id.Value << " have types that do not meet — `then` is ";
        ThenValue->getType()->print( Out );
        Out << ", `else` is ";
        ElseValue->getType()->print( Out );
        Out << ", and the expression's own is ";
        Shape->print( Out );
        static_cast<void>( Emitter.Fail( Report ) );
        return nullptr;
    }

    llvm::PHINode *Result = Builder.CreatePHI( Shape, 2, "ternary" );
    Result->addIncoming( ThenValue, ThenEnd );
    Result->addIncoming( ElseValue, ElseEnd );
    return Result;
}
// NOLINTEND(clang-analyzer-security.ArrayBound)
