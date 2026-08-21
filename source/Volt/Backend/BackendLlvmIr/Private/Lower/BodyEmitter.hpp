#pragma once

// BodyEmitter.hpp — one function body being lowered.
//
// Constructed per body, over a FunctionFrame the caller owns, and destroyed
// when that body is finished. It replaces the old arrangement where the frame
// was a *member* of the emitter state, cleared by assignment at both ends of
// every body:
//
//     Frame = FunctionFrame{};  Frame.Fn = Fn;  …  EmitStmts( … );  Frame = FunctionFrame{};
//
// became
//
//     FunctionFrame Frame;  Frame.Fn = Fn;  …
//     BodyEmitter Emitter( Services, Frame );  Emitter.EmitStmts( Body, Frame.bReturnsValue );
//
// Not virtual, and deliberately so: per-node dispatch stays one
// `std::visit( Meta::Overloaded{ … } )` site per category (core-interfaces.md),
// never a class hierarchy per node kind. The visit arms themselves are named
// free functions declared in Stmt/StmtEmitter.hpp and Expr/ExprEmitter.hpp, so
// this header stays a small mutual-recursion core rather than the ~90-method
// god header it replaced.

#include "Volt/BackendCore/BackendInput.hpp"
#include "Volt/BackendCore/TargetBackend.hpp"

#include "Core/LlvmFwd.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace Volt
{

namespace Backend
{

    class DiagnosticSink;

    namespace Llvm
    {

        class ModuleContext;
        class TypeMapper;
        struct EmitterServices;
        struct FunctionFrame;

        class BodyEmitter final
        {

        public:

            // Registers the frame's function name with the diagnostic sink for
            // exactly this body's lifetime, which is what appends
            // "(while emitting '<symbol>')" to whatever it reports.
            BodyEmitter ( EmitterServices &InServices, FunctionFrame &InFrame );
            ~BodyEmitter ();

            BodyEmitter ( const BodyEmitter & )           = delete;
            BodyEmitter &operator=( const BodyEmitter & ) = delete;

            // --- Access ------------------------------------------------------

            [[nodiscard]] EmitterServices &Services () const noexcept
            {
                return *ServicesPtr;
            }

            [[nodiscard]] FunctionFrame &Frame () const noexcept
            {
                return *FramePtr;
            }

            [[nodiscard]] ModuleContext &Ctx () const noexcept;
            [[nodiscard]] DiagnosticSink &Diag () const noexcept;
            [[nodiscard]] TypeMapper &Types () const noexcept;

            [[nodiscard]] bool Failed () const noexcept;
            [[nodiscard]] bool Terminated () const noexcept;
            EEmitStatus Fail ( std::string Message );

            // Frame-bound shorthands for the type service, which are what most
            // emitters actually call.
            [[nodiscard]] MiddleEnd::TypeSystem::LayoutId LayoutOfExpr ( Frontend::ExprId Id ) const;
            [[nodiscard]] llvm::Type *TypeOfExpr ( Frontend::ExprId Id ) const;
            [[nodiscard]] bool IsAggregate ( MiddleEnd::TypeSystem::LayoutId Id ) const;

            // --- Statements (Stmt/) ------------------------------------------

            // Emit a statement list. `bTail` marks the list as the function's
            // result position, where a trailing expression *is* the return value.
            void EmitStmts ( const Frontend::StmtList &List, bool bTail );
            void EmitStmt ( Frontend::StmtId Id, bool bTail );

            // --- Expressions (Expr/) -----------------------------------------

            // The value of an expression: a register for a scalar, a `ptr` to
            // the storage for an aggregate. Null on failure, with the
            // diagnostic already set.
            [[nodiscard]] llvm::Value *EmitExpr ( Frontend::ExprId Id );

            // The *address* of an assignable expression — a local slot, an `@x`
            // GEP, a field GEP, the operand of a `*p`. Null when the expression
            // is not a place, which is a middle-end contract violation and is
            // reported.
            [[nodiscard]] llvm::Value *EmitAddress ( Frontend::ExprId Id );

            // Address of a place, then the load of it.
            [[nodiscard]] llvm::Value *LoadPlace ( Frontend::ExprId Id );

            // Move a value into storage: a plain store for a scalar, a memcpy
            // sized by LayoutEngine for an aggregate, since an aggregate is only
            // ever an address.
            void EmitStore ( llvm::Value *Address, llvm::Value *Value, MiddleEnd::TypeSystem::LayoutId Shape );

            // GEP to the field named `Name` inside an aggregate.
            [[nodiscard]] llvm::Value *FieldAddress ( llvm::Value *Object,
                                                      MiddleEnd::TypeSystem::LayoutId Shape,
                                                      std::string_view Name,
                                                      Frontend::ExprId Id );

            // `@name` as written, reduced to the field name declared in the
            // layout. Sema strips the sigil in one place (LookupOn's CleanName)
            // and this is the other half of the same contract.
            [[nodiscard]] static std::string_view FieldNameOf ( std::string_view Written );

            // The report for a receiver-less `self` / `super` / `@x`.
            [[nodiscard]] std::string MissingSelf ( std::string_view What ) const;

            // --- Storage ------------------------------------------------------

            // The storage backing a binding, created in the entry block on first
            // mention so a `LocalDecl` and every later use of it share one slot.
            [[nodiscard]] llvm::Value *
            SlotFor ( const MiddleEnd::Resolver::BindingSite &Site, llvm::Type *Shape, std::string_view Name );

            // Unnamed entry-block storage, for a value that converges out of
            // several blocks and has no binding to key on.
            [[nodiscard]] llvm::AllocaInst *MakeTemp ( llvm::Type *Shape, std::string_view Name ) const;

            // A statement list's trailing expression converging into a result
            // slot. `if`, `begin` and `case` all do this and must do it
            // identically, so the rule lives here once rather than at the three
            // stores.
            void
            StoreTailValue ( llvm::Value *Value, llvm::Value *Slot, llvm::Type *Shape, MiddleEnd::TypeSystem::LayoutId Layout );

            // The value a slot-converging expression hands back to its consumer.
            // The counterpart of StoreTailValue, obeying the same convention: an
            // aggregate is only ever an *address*, so the slot itself is the
            // value; a scalar is loaded out of it.
            [[nodiscard]] llvm::Value *
            LoadConverged ( llvm::Value *Slot, llvm::Type *Shape, MiddleEnd::TypeSystem::LayoutId Layout, const char *Name );

            // The zext TypeCompat's widening rule implies. Sema already decided
            // an `i8` is acceptable where a `u64` is expected
            // (rules/zero-hardcode.md); this only carries that decision onto the
            // wire.
            [[nodiscard]] llvm::Value *CoerceWidth ( llvm::Value *Value, llvm::Type *Target ) const;

            // --- Calls (Expr/Expr*CallEmitter.cpp) ----------------------------

            [[nodiscard]] llvm::Value *EmitCall ( Frontend::ExprId Id, const Frontend::Call &Node );

            // The one call site every resolved callee goes through — an explicit
            // `Call`, and a `Binary`/`Unary` whose operator resolved to a method
            // (rules/core-ast.md: the two are the same emission, told apart only
            // by where the receiver and the operands come from). `Block` is the
            // trailing `do ... end`, which fills the callee's `&block` slot
            // rather than a positional one and is invalid for every other caller.
            [[nodiscard]] llvm::Value *EmitResolvedCall ( Frontend::ExprId Id,
                                                          const MiddleEnd::IR::CalleeEntry &Entry,
                                                          Frontend::ExprId Receiver,
                                                          std::span<const Frontend::ExprId> Args,
                                                          Frontend::ExprId Block = Frontend::ExprId{} );

        private:

            EmitterServices *ServicesPtr = nullptr;
            FunctionFrame *FramePtr      = nullptr;
        };

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
