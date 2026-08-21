// ExprBinaryEmitter.cpp — `a op b`.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/Expr/InstructionTables.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include "Volt/Frontend/Lexer/Token.hpp"

#include <llvm/IR/IRBuilder.h>

#include <span>
#include <string>

// NOLINTBEGIN(clang-analyzer-security.ArrayBound) — false positive via LLVM's hung-off operand layout
llvm::Value *Volt::Backend::Llvm::EmitBinary ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::Binary &Node )
{
    // The protocol, in the one order that keeps a backend free of member lookup
    // (rules/core-ast.md): the resolution first, the instruction only when there
    // is none.
    if ( const MiddleEnd::IR::CalleeEntry *Entry = ResolvedOperator( Emitter, Id ); Entry != nullptr )
    {
        return Emitter.EmitResolvedCall( Id, *Entry, Node.Lhs, std::span<const Frontend::ExprId>{ &Node.Rhs, 1 } );
    }

    // `and` / `or` are spelled operators that short-circuit, so they are control
    // flow and never a table row.
    if ( Node.Op == Frontend::TokenKind::KwAnd or Node.Op == Frontend::TokenKind::AndAnd or
         Node.Op == Frontend::TokenKind::KwOr or Node.Op == Frontend::TokenKind::OrOr )
    {
        return EmitShortCircuit( Emitter, Node );
    }

    const MiddleEnd::TypeSystem::LayoutId Shape = Emitter.LayoutOfExpr( Node.Lhs );
    const std::string_view Spelling             = Emitter.Types().SpellingOf( Shape );
    const EOpFamily Family                      = FamilyOf( Spelling );
    if ( Family == EOpFamily::None )
    {
        static_cast<void>( Emitter.Fail( "llvm: operator '" + std::string( Frontend::TokenSpelling( Node.Op ) ) +
                                         "' is neither resolved to a method nor carried by a primitive receiver" ) );
        return nullptr;
    }

    // Pointer arithmetic is heterogeneous — `( offset : UInt64 ) -> Pointer<T>`
    // declared on the pointer nominal itself — so it is an address computation
    // with the pointee's stride, not an opcode.
    if ( Spelling == "ptr" and ( Node.Op == Frontend::TokenKind::Plus or Node.Op == Frontend::TokenKind::Minus ) )
    {
        return EmitPointerArith( Emitter, Node );
    }

    llvm::Value *Lhs = Emitter.EmitExpr( Node.Lhs );
    llvm::Value *Rhs = Emitter.EmitExpr( Node.Rhs );
    if ( Lhs == nullptr or Rhs == nullptr )
    {
        return nullptr;
    }

    llvm::IRBuilder<> &Builder = Emitter.Ctx().Builder();

    auto CoerceToSame = [&] ( llvm::Value *V, llvm::Type *T ) -> llvm::Value *
    {
        if ( V->getType() == T )
        {
            return V;
        }
        if ( V->getType()->isIntegerTy() and T->isIntegerTy() )
        {
            if ( V->getType()->getIntegerBitWidth() < T->getIntegerBitWidth() )
            {
                return Builder.CreateZExt( V, T );
            }
            if ( V->getType()->getIntegerBitWidth() > T->getIntegerBitWidth() )
            {
                return Builder.CreateTrunc( V, T );
            }
        }
        if ( V->getType()->isIntegerTy() and T->isFloatingPointTy() )
        {
            return Builder.CreateSIToFP( V, T );
        }
        if ( V->getType()->isFloatingPointTy() and T->isFloatingPointTy() )
        {
            if ( V->getType()->isFloatTy() and T->isDoubleTy() )
            {
                return Builder.CreateFPExt( V, T );
            }
            if ( V->getType()->isDoubleTy() and T->isFloatTy() )
            {
                return Builder.CreateFPTrunc( V, T );
            }
        }
        return Emitter.CoerceWidth( V, T );
    };

    if ( const BinOpRow *Row = FindBinOp( Family, Node.Op ); Row != nullptr )
    {
        return Builder.CreateBinOp( EncodingOf( Row->Opcode ), Lhs, CoerceToSame( Rhs, Lhs->getType() ) );
    }
    if ( const CmpRow *Row = FindCmp( Family, Node.Op ); Row != nullptr )
    {
        const llvm::CmpInst::Predicate Predicate = EncodingOf( Row->Predicate );
        return Family == EOpFamily::Float ? Builder.CreateFCmp( Predicate, Lhs, CoerceToSame( Rhs, Lhs->getType() ) )
                                          : Builder.CreateICmp( Predicate, Lhs, CoerceToSame( Rhs, Lhs->getType() ) );
    }

    static_cast<void>( Emitter.Fail( "llvm: no machine instruction for '" + std::string( Frontend::TokenSpelling( Node.Op ) ) +
                                     "' on a '" + std::string( Spelling ) + "' receiver" ) );
    return nullptr;
}
// NOLINTEND(clang-analyzer-security.ArrayBound)
