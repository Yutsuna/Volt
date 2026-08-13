// ExprUnaryEmitter.cpp — `op a`.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"

#include "Volt/Frontend/Lexer/Token.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <string>

llvm::Value *Volt::Backend::Llvm::EmitUnary ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::Unary &Node )
{
    if ( const MiddleEnd::IR::CalleeEntry *Entry = ResolvedOperator( Emitter, Id ); Entry != nullptr )
    {
        return Emitter.EmitResolvedCall( Id, *Entry, Node.Operand, {} );
    }

    const std::string_view Spelling = Emitter.Types().SpellingOf( Emitter.LayoutOfExpr( Node.Operand ) );
    const EOpFamily Family          = FamilyOf( Spelling );
    const UnOpRow *Row              = FindUnOp( Family, Node.Op );
    if ( Row == nullptr )
    {
        static_cast<void>( Emitter.Fail( "llvm: no machine instruction for unary '" +
                                         std::string( Frontend::TokenSpelling( Node.Op ) ) + "' on a '" +
                                         std::string( Spelling ) + "' operand" ) );
        return nullptr;
    }

    llvm::Value *Operand = Emitter.EmitExpr( Node.Operand );
    if ( Operand == nullptr )
    {
        return nullptr;
    }

    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();

    switch ( Row->Kind )
    {
    case EUnaryOp::Neg:
        return Builder.CreateNeg( Operand );
    case EUnaryOp::FNeg:
        return Builder.CreateFNeg( Operand );
    case EUnaryOp::BitNot:
        return Builder.CreateNot( Operand );
    case EUnaryOp::LogicalNot:
        // `xor true` on the one-bit shape, which is what `not` means; on a wider
        // integer the same row would silently mean something else, so it is
        // refused rather than reinterpreted.
        if ( not Operand->getType()->isIntegerTy( 1 ) )
        {
            static_cast<void>( Emitter.Fail( "llvm: logical '" + std::string( Frontend::TokenSpelling( Node.Op ) ) + "' on a '" +
                                             std::string( Spelling ) + "' operand that is not one bit wide" ) );
            return nullptr;
        }
        return Builder.CreateXor( Operand, Builder.getTrue() );
    }
    return nullptr;
}
