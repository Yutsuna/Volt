// ExprCallEmitter.cpp — the `Call` node itself.
//
// Thin by design: all it does is find the receiver and hand off. Every resolved
// callee — an explicit `Call`, and a `Binary`/`Unary` whose operator resolved to
// a method — goes through the one site in ExprResolvedCallEmitter.cpp, told
// apart only by where the receiver and the operands come from
// (rules/core-ast.md).

#include "Lower/Expr/ExprEmitter.hpp"

#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <span>
#include <string>
#include <variant>

llvm::Value *Volt::Backend::Llvm::BodyEmitter::EmitCall ( Frontend::ExprId Id, const Frontend::Call &Node )
{
    Frontend::ExprId CalleeId = Node.Callee;
    if ( Frame().Redirects != nullptr )
    {
        if ( const auto It = Frame().Redirects->find( CalleeId.Value ); It != Frame().Redirects->end() )
        {
            CalleeId = It->second;
        }
    }

    const MiddleEnd::IR::CalleeEntry *Entry = Frame().Callees->Get( CalleeId );
    if ( Entry == nullptr or Entry->Decl == nullptr )
    {
        Entry = Frame().Callees->Get( Node.Callee );
    }
    if ( Entry == nullptr or Entry->Decl == nullptr )
    {
        static_cast<void>( Fail( "llvm: call at expression " + std::to_string( Id.Value ) +
                                 " carries no callee resolution — TypeChecker records one for every call it accepts" ) );
        return nullptr;
    }

    // The receiver is the callee's own object, when the callee is a member
    // access. A free function's callee is an Identifier and has none — unless
    // the resolution is indirect, where the callee expression *is* the callable
    // being invoked: `f( x )` on a local holding a closure.
    Frontend::ExprId Receiver;
    if ( const auto *Access = std::get_if<Frontend::Member>( &Frame().Unit->Ast->Expr( CalleeId ) ); Access != nullptr )
    {
        Receiver = Access->Object;
    }
    else if ( Entry->bIndirect )
    {
        Receiver = CalleeId;
    }

    return EmitResolvedCall( Id, *Entry, Receiver, std::span<const Frontend::ExprId>{ Node.Args.begin(), Node.Args.Size() },
                             Node.BlockArg );
}
