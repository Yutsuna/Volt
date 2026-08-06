// BlockNextEmitter.cpp — `next` inside a lifted closure body with no loop.
//
// The block ends this invocation and hands its value back, so it is a `ret`, not
// a branch. Kept with the closure protocol rather than with the loop one: the
// two only share a keyword.

#include "Lower/Closure/ClosureLowering.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>

void Volt::Backend::Llvm::ClosureLowering::EmitBlockNext ( BodyEmitter &Emitter, Frontend::ExprId Value )
{
    FunctionFrame &Frame       = Emitter.Frame();
    llvm::IRBuilder<> &Builder = Services->Ctx->Builder();

    if ( not Frame.bReturnsValue )
    {
        if ( Value.IsValid() )
        {
            static_cast<void>( Emitter.Fail( "llvm: `next` carries a value out of a closure whose type declares no result" ) );
            return;
        }
        static_cast<void>( Builder.CreateRetVoid() );
        return;
    }

    if ( not Value.IsValid() )
    {
        // A bare `next` in a value-producing block: the same hole a body falling
        // off its end leaves, and lowered the same way.
        static_cast<void>( Builder.CreateUnreachable() );
        return;
    }

    if ( llvm::Value *Result = Emitter.EmitExpr( Value ); Result != nullptr )
    {
        static_cast<void>( Builder.CreateRet( Emitter.CoerceWidth( Result, Frame.Fn->getReturnType() ) ) );
    }
}
