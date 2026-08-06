// ExprControlEmitter.cpp — the arm shape `if` and `case` share.
//
// Both are slot-converging expressions whose arms are *statement lists*, so the
// number of blocks reaching the merge is only known once the whole chain has
// been built — which rules out a phi. Each arm therefore: emits its statements,
// stores its trailing expression into the convergence slot, and branches to the
// merge unless it already terminated.
//
// That was written twice, character for character, as a local lambda in each
// emitter. It is one function now — not to save nine lines, but because a
// divergence between the two is exactly the kind of bug neither of them would
// report: the arms would simply converge differently.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/IRBuilder.h>

#include <variant>

void Volt::Backend::Llvm::EmitConvergingBody ( BodyEmitter &Emitter,
                                               const Frontend::StmtList &Body,
                                               llvm::AllocaInst *Slot,
                                               llvm::Type *Shape,
                                               Sema::LayoutId Layout,
                                               llvm::BasicBlock *Merge )
{
    const Frontend::AstContext &Ast = *Emitter.Frame().Unit->Ast;

    // The arm's value is its trailing expression, stored rather than returned —
    // the same tail rule the function body uses.
    for ( std::size_t Index = 0; Index < Body.Size() and not Emitter.Terminated(); ++Index )
    {
        const bool bLast = Index + 1 == Body.Size();
        if ( bLast and Slot != nullptr )
        {
            if ( const auto *Tail = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Body[Index] ) ); Tail != nullptr )
            {
                Emitter.StoreTailValue( Emitter.EmitExpr( Tail->Expr ), Slot, Shape, Layout );
                continue;
            }
        }
        Emitter.EmitStmt( Body[Index], false );
    }
    if ( not Emitter.Terminated() )
    {
        static_cast<void>( Emitter.Ctx().Builder().CreateBr( Merge ) );
    }
}
