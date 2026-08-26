#pragma once

// ExprEmitter.hpp — the value-producing core nodes.
//
// One `std::visit` site for the whole category (core-interfaces.md), in
// ExprEmitter.cpp: per-node dispatch is never virtual and never a `switch` over
// kinds, and a node the contract says cannot reach a backend falls into the
// `auto` arm and is reported by name rather than guessed at. The arms are the
// named functions below rather than inline lambdas, so the visit site stays a
// table of one-liners and each arm lives in the file its name says it does.
//
// Two conventions hold throughout, and everything else follows from them:
//
//   - A scalar evaluates to a register; an **aggregate evaluates to a `ptr`**
//     at its storage (abi.md: aggregates by pointer). So `EmitExpr` on a
//     struct-shaped expression hands back an address, and `EmitStore` moves it
//     with a memcpy sized by LayoutEngine.
//   - Nothing here decides a type. Every shape comes from `Values->ExprType`
//     through InstanceLayouts, every callee from `Callees->Get`, every binding
//     from `Scopes->BindingOf`. When one of those is missing the emitter says
//     so and stops — a middle-end whose own invariants (rules/core-ast.md) say
//     it cannot happen is worth a loud report, never a repair.

#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/InstructionSchema.hpp"

#include "Core/LlvmFwd.hpp"

#include <span>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class BodyEmitter;

        // --- Literals (ExprLiteralEmitter.cpp) ---------------------------------
        [[nodiscard]] llvm::Value *EmitIntLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::IntLiteral &Node );
        [[nodiscard]] llvm::Value *
        EmitFloatLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::FloatLiteral &Node );
        [[nodiscard]] llvm::Value *
        EmitCharLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::CharLiteral &Node );
        [[nodiscard]] llvm::Value *
        EmitStringLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::StringLiteral &Node );
        [[nodiscard]] llvm::Value *
        EmitBoolLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::BoolLiteral &Node );
        [[nodiscard]] llvm::Value *EmitNilLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id );
        [[nodiscard]] llvm::Value *
        EmitSymbolLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::SymbolLiteral &Node );
        [[nodiscard]] llvm::Value *EmitSizeOf ( BodyEmitter &Emitter, Frontend::ExprId Id );
        [[nodiscard]] llvm::Value *EmitTypeTrait ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::TypeTrait &Node );

        // --- Access (ExprAccessEmitter.cpp) ------------------------------------

        // A paren-less, receiver-less call is a bare `Identifier`, exactly as a
        // paren-less `Member` is; only the resolution tells either apart from a
        // place read, so both consult Callees first and fall back to LoadPlace.
        [[nodiscard]] llvm::Value *EmitIdentifierValue ( BodyEmitter &Emitter, Frontend::ExprId Id );
        [[nodiscard]] llvm::Value *EmitMemberValue ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::Member &Node );
        [[nodiscard]] llvm::Value *EmitSelf ( BodyEmitter &Emitter );
        [[nodiscard]] llvm::Value *EmitSuper ( BodyEmitter &Emitter );
        [[nodiscard]] llvm::Value *EmitFuncAddr ( BodyEmitter &Emitter, const Frontend::FuncAddr &Node );

        // --- Trait objects (ExprDynamicUpcast.cpp) -----------------------------

        // `Concrete` where a trait is expected: builds the `{ ptr data, ptr
        // vtable }` pair and yields its address, like any other aggregate.
        [[nodiscard]] llvm::Value *
        EmitDynamicUpcast ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::DynamicUpcast &Node );

        // --- Operations --------------------------------------------------------
        [[nodiscard]] llvm::Value *EmitAssign ( BodyEmitter &Emitter, const Frontend::Assign &Node );

        // --- Operators ---------------------------------------------------------

        // Shared by EmitBinary and EmitUnary (ExprOperatorEmitter.cpp): both ask
        // the same two questions in the same order, and answering them
        // differently in the two files is precisely the drift these prevent.

        // The method this operator resolved to, or null when the receiver's
        // layout is what supplies it. Reading this *first* is what keeps member
        // lookup out of the backend.
        [[nodiscard]] const MiddleEnd::IR::CalleeEntry *ResolvedOperator ( BodyEmitter &Emitter, Frontend::ExprId Id );

        // The instruction family of an expression's own receiver layout.
        [[nodiscard]] EOpFamily FamilyOfExpr ( BodyEmitter &Emitter, Frontend::ExprId Id );

        [[nodiscard]] llvm::Value *EmitBinary ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::Binary &Node );
        [[nodiscard]] llvm::Value *EmitUnary ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::Unary &Node );
        [[nodiscard]] llvm::Value *EmitPointerArith ( BodyEmitter &Emitter, const Frontend::Binary &Node );
        [[nodiscard]] llvm::Value *EmitShortCircuit ( BodyEmitter &Emitter, const Frontend::Binary &Node );

        // --- Control -----------------------------------------------------------
        [[nodiscard]] llvm::Value *EmitTernary ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::Ternary &Node );
        [[nodiscard]] llvm::Value *EmitCase ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::CaseExpr &Node );
        [[nodiscard]] llvm::Value *EmitIf ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::If &Node );

        // The arm shape `if` and `case` share exactly: emit a statement list,
        // store its trailing expression into the convergence slot, and branch to
        // the merge unless the arm already terminated. Extracted because the two
        // were character-for-character identical, and a divergence between them
        // is precisely the kind of bug neither reports.
        void EmitConvergingBody ( BodyEmitter &Emitter,
                                  const Frontend::StmtList &Body,
                                  llvm::AllocaInst *Slot,
                                  llvm::Type *Shape,
                                  MiddleEnd::TypeSystem::LayoutId Layout,
                                  llvm::BasicBlock *Merge );

        // --- Calls -------------------------------------------------------------

        // The `bIndirect` arm of EmitResolvedCall: a callable value has no body
        // and no symbol, so the call goes through its `{ code, env }` pair. The
        // emission itself belongs to ClosureLowering; what lives here is the
        // one rule that is the *expression* layer's, namely that a trailing
        // block cannot be passed to a callable.
        [[nodiscard]] llvm::Value *EmitIndirectDispatch ( BodyEmitter &Emitter,
                                                          Frontend::ExprId Id,
                                                          const MiddleEnd::IR::CalleeEntry &Entry,
                                                          Frontend::ExprId Receiver,
                                                          std::span<const Frontend::ExprId> Args,
                                                          Frontend::ExprId Block );

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
