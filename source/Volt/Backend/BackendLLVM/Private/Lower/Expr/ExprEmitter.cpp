// ExprEmitter.cpp — the one std::visit site for the expression category.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/EmitterServices.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/Exception/ExceptionLowering.hpp"
#include "Lower/FunctionFrame.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

#include <string>
#include <variant>

llvm::Value *Volt::Backend::Llvm::BodyEmitter::EmitExpr ( Frontend::ExprId Id )
{
    // `Terminated()` here as well as in `EmitStmts`: a `raise` (or a call the
    // post-call check found pending) can end the current block mid-expression —
    // e.g. a non-tail argument — and every remaining sub-expression of the one
    // being built is genuinely unreachable. Nothing here decides that; it only
    // refuses to append instructions after it.
    if ( Failed() or Terminated() or Frame().Unit == nullptr or not Id.IsValid() )
    {
        return nullptr;
    }

    const Frontend::AstContext &Ast = *Frame().Unit->Ast;

    return std::visit(
        Meta::Overloaded{
            // --- Terminals -------------------------------------------------
            [this, Id] ( const Frontend::IntLiteral &Node ) -> llvm::Value * { return EmitIntLiteral( *this, Id, Node ); },
            [this, Id] ( const Frontend::FloatLiteral &Node ) -> llvm::Value * { return EmitFloatLiteral( *this, Id, Node ); },
            [this, Id] ( const Frontend::CharLiteral &Node ) -> llvm::Value * { return EmitCharLiteral( *this, Id, Node ); },
            [this, Id] ( const Frontend::BoolLiteral &Node ) -> llvm::Value * { return EmitBoolLiteral( *this, Id, Node ); },
            [this, Id] ( const Frontend::NilLiteral & ) -> llvm::Value * { return EmitNilLiteral( *this, Id ); },
            [this, Id] ( const Frontend::StringLiteral &Node ) -> llvm::Value * { return EmitStringLiteral( *this, Id, Node ); },
            [this, Id] ( const Frontend::SymbolLiteral &Node ) -> llvm::Value * { return EmitSymbolLiteral( *this, Id, Node ); },

            // --- Access ----------------------------------------------------
            [this, Id] ( const Frontend::Identifier & ) -> llvm::Value * { return EmitIdentifierValue( *this, Id ); },
            [this, Id] ( const Frontend::InstanceVar & ) -> llvm::Value * { return LoadPlace( Id ); },
            [this, Id] ( const Frontend::Member &Node ) -> llvm::Value * { return EmitMemberValue( *this, Id, Node ); },
            [this, Id] ( const Frontend::Deref & ) -> llvm::Value * { return LoadPlace( Id ); },
            [this] ( const Frontend::SelfExpr & ) -> llvm::Value * { return EmitSelf( *this ); },
            // `super` is the same receiver: which body it reaches was decided by
            // Sema and travels in the call's CalleeEntry, not in the value.
            [this] ( const Frontend::SuperExpr & ) -> llvm::Value * { return EmitSuper( *this ); },

            // --- Operations ------------------------------------------------
            [this, Id] ( const Frontend::Call &Node ) -> llvm::Value * { return EmitCall( Id, Node ); },
            [this] ( const Frontend::Assign &Node ) -> llvm::Value * { return EmitAssign( *this, Node ); },
            [this, Id] ( const Frontend::Ternary &Node ) -> llvm::Value * { return EmitTernary( *this, Id, Node ); },

            // --- Operators -------------------------------------------------
            [this, Id] ( const Frontend::Binary &Node ) -> llvm::Value * { return EmitBinary( *this, Id, Node ); },
            [this, Id] ( const Frontend::Unary &Node ) -> llvm::Value * { return EmitUnary( *this, Id, Node ); },

            // --- Control ---------------------------------------------------
            [this, Id] ( const Frontend::CaseExpr &Node ) -> llvm::Value * { return EmitCase( *this, Id, Node ); },
            [this, Id] ( const Frontend::If &Node ) -> llvm::Value * { return EmitIf( *this, Id, Node ); },
            [this, Id] ( const Frontend::BeginExpr &Node ) -> llvm::Value *
            { return Services().Exceptions->EmitBegin( *this, Id, Node ); },
            [this, Id] ( const Frontend::RaiseExpr &Node ) -> llvm::Value *
            { return Services().Exceptions->EmitRaise( *this, Id, Node ); },

            // --- Inert -----------------------------------------------------
            // Neither carries a runtime value, and neither is descended into
            // (rules/core-ast.md). GenericInst is a *spelling* of a type in
            // value position — the Call wrapping it is what emits anything.
            [] ( const Frontend::GenericInst & ) -> llvm::Value * { return nullptr; },
            [this, Id] ( const Frontend::SizeOf & ) -> llvm::Value * { return EmitSizeOf( *this, Id ); },
            [this] ( const Frontend::FuncAddr &Node ) -> llvm::Value * { return EmitFuncAddr( *this, Node ); },

            // `( Value : Type )` — TypeChecker already constrained `Value` to
            // `Type` and stamped both with the same SemaTypeId (core-ast.md), so
            // by codegen it is a pure passthrough: no instruction of its own,
            // just `Value`'s.
            [this] ( const Frontend::TypedExpr &Node ) -> llvm::Value * { return EmitExpr( Node.Value ); },

            [this, Id] ( const auto & ) -> llvm::Value *
            {
                // Sugar, by elimination: AstInvariant (order 40) proves none
                // survives Lowering, so reaching this is a middle-end bug and is
                // named as one.
                static_cast<void>( Fail( "llvm: " + std::string( Frontend::NodeName( Frame().Unit->Ast->Expr( Id ) ) ) +
                                         " reached codegen — no Lowering pass removed it" ) );
                return nullptr;
            } },
        Ast.Expr( Id ) );
}

llvm::Value *Volt::Backend::Llvm::EmitAssign ( BodyEmitter &Emitter, const Frontend::Assign &Node )
{
    llvm::Value *Value = Emitter.EmitExpr( Node.Value );
    llvm::Value *Place = Emitter.EmitAddress( Node.Target );
    if ( Value == nullptr or Place == nullptr )
    {
        return nullptr;
    }
    // AssignLowering (order 24) desugared every compound form, so exactly one
    // shape of Assign reaches here: a store.
    Emitter.EmitStore( Place, Value, Emitter.LayoutOfExpr( Node.Target ) );
    return Value;
}
