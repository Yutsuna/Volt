// ExprPointerArith.cpp — `p + n` / `p - n`.
//
// Heterogeneous by declaration (`( offset : UInt64 ) -> Pointer<T>`, declared on
// the pointer nominal itself), so it is an address computation with the
// pointee's stride rather than an arithmetic opcode — which is why it has no row
// in InstructionTables and its own file here.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include "Volt/Frontend/Lexer/Token.hpp"

#include <llvm/IR/IRBuilder.h>

llvm::Value *Volt::Backend::Llvm::EmitPointerArith ( BodyEmitter &Emitter, const Frontend::Binary &Node )
{
    // The pointee is the first generic argument of whichever stdlib type claimed
    // PointerType — "Pointer" is not a name this compiler knows
    // (rules/core-ast.md).
    const MiddleEnd::TypeSystem::UnitTypes &Values   = *Emitter.Frame().Values;
    const MiddleEnd::TypeSystem::SemaTypeId Receiver = Values.ExprType( Node.Lhs );
    if ( not Values.Has( Receiver ) or Values.Get( Receiver ).Args.Size() == 0 )
    {
        static_cast<void>( Emitter.Fail( "llvm: pointer arithmetic on a receiver with no pointee argument" ) );
        return nullptr;
    }

    llvm::Type *Pointee = Emitter.Types().TypeOfLayout( Emitter.Types().LayoutOfValue( Values, Values.Get( Receiver ).Args[0] ) );
    if ( Pointee == nullptr )
    {
        static_cast<void>( Emitter.Fail( "llvm: pointer arithmetic whose pointee has no resolved layout" ) );
        return nullptr;
    }

    llvm::Value *Base   = Emitter.EmitExpr( Node.Lhs );
    llvm::Value *Offset = Emitter.EmitExpr( Node.Rhs );
    if ( Base == nullptr or Offset == nullptr )
    {
        return nullptr;
    }

    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();

    Offset = Emitter.CoerceWidth( Offset, Builder.getInt64Ty() );
    if ( Node.Op == Frontend::TokenKind::Minus )
    {
        Offset = Builder.CreateNeg( Offset );
    }
    return Builder.CreateGEP( Pointee, Base, Offset );
}
