// StmtEmitter.cpp — the statement category, and the tail rule that makes an
// implicit return work without any type inference.
//
// Volt has no `return` requirement: `Int8#<=>` ends in an if/elsif/else chain
// and its value is the value of whichever branch ran. That is a *structural*
// property, not a typing one, so it is decided here by position alone:
//
//   EmitStmts( List, bTail ) marks the last statement of a list as being in
//   result position, and exactly one node kind does anything with it — an
//   ExprStmt emits `ret`.
//
// No other node propagates it, because no other node's value can be the
// function's: a `while` has none, and a `return` already is one. `If` used to
// be the second reader, forwarding the flag into both branches; now that it is
// an expression (Expr.hpp) it converges its branches into a slot and hands the
// loaded value back to the enclosing ExprStmt, which emits the single `ret`.
// The rule therefore needs nothing from Sema, which is what lets it live in a
// backend at all — see .agents/backend/llvm.md for why it arguably belongs in
// a Lowering instead.
//
// One `std::visit` for the whole category, like ExprEmitter: a statement the
// contract says cannot reach a backend falls into the `auto` arm and is
// reported by name.

#include "LlvmState.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>

#include <string>
#include <variant>

bool Volt::Backend::Llvm::LlvmBackend::State::Terminated () const
{
    llvm::BasicBlock *Block = Builder->GetInsertBlock();
    return Block == nullptr or Block->getTerminator() != nullptr;
}

llvm::AllocaInst *Volt::Backend::Llvm::LlvmBackend::State::MakeTemp ( llvm::Type *Shape, std::string_view Name ) const
{
    if ( Shape == nullptr or Frame.Entry == nullptr )
    {
        return nullptr;
    }

    // Every alloca goes in the entry block, whatever block the walk is in when
    // it needs one: that is the precondition mem2reg promotes on, and it is why
    // this emitter never builds SSA itself.
    llvm::IRBuilder<> Entry{ Frame.Entry, Frame.Entry->begin() };
    return Entry.CreateAlloca( Shape, nullptr, Name );
}

void Volt::Backend::Llvm::LlvmBackend::State::StoreTailValue ( llvm::Value *Value,
                                                               llvm::Value *Slot,
                                                               llvm::Type *Shape,
                                                               Sema::LayoutId Layout )
{
    // A tail expression with no value converges nothing. The ordinary case is a
    // call to a `-> Void` member: `begin level3() rescue e : E then 7 end` in
    // statement position is valid Volt where only the rescue arm has a value,
    // so the slot is simply never written on this path. Nothing to report.
    //
    // It is also not something to hand to CreateStore. A void operand builds an
    // ill-formed instruction, and LLVM answers that by asking its DataLayout
    // for the alignment of a type that has none — which is not a diagnostic but
    // an unbounded scan inside the library. That was a compiler hang, on a
    // fifteen-line program.
    if ( Value == nullptr or Slot == nullptr or Shape == nullptr or Value->getType()->isVoidTy() )
    {
        return;
    }

    // An aggregate value is only ever an address (rules/core-ast.md): the arm
    // handed back the address of its own storage, and converging it into the
    // slot is `EmitStore`'s ordinary memcpy, exactly like assigning one String
    // to another — never a load into an SSA value of the struct type.
    if ( IsAggregate( Layout ) )
    {
        EmitStore( Slot, Value, Layout );
        return;
    }

    // Any *other* disagreement is a real one: the slot's type is the one Sema
    // gave the whole `begin`/`case`, so a value that does not fit it after
    // coercion means the arms were typed inconsistently. Named here rather than
    // stored through an opaque pointer, which is exactly how a mismatched width
    // becomes silent corruption (the reason EmitStore reconciles centrally).
    llvm::Value *Fitted = CoerceWidth( Value, Shape );
    if ( Fitted == nullptr or Fitted->getType() != Shape )
    {
        std::string ValueText;
        std::string SlotText;
        llvm::raw_string_ostream ValueStream( ValueText );
        llvm::raw_string_ostream SlotStream( SlotText );
        ( Fitted != nullptr ? Fitted->getType() : Value->getType() )->print( ValueStream );
        Shape->print( SlotStream );
        static_cast<void>(
            Fail( "llvm: a `begin`/`case` arm yields " + ValueStream.str() + " but the result slot is " + SlotStream.str() +
                  " — the arms of one expression were typed inconsistently (fn: " + Frame.Fn->getName().str() + ")" ) );
        return;
    }
    static_cast<void>( Builder->CreateStore( Fitted, Slot ) );
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::LoadConverged ( llvm::Value *Slot,
                                                                      llvm::Type *Shape,
                                                                      Sema::LayoutId Layout,
                                                                      const char *Name )
{
    if ( Slot == nullptr or Shape == nullptr )
    {
        return nullptr;
    }

    // Loading an aggregate out of the slot would produce an SSA struct, and
    // every consumer of an aggregate expects an address instead: `EmitStore`
    // memcpys from it, a call passes it byval. Handing back the loaded struct
    // built a memcpy whose operand was `{ ptr, i64 }` rather than `ptr`, which
    // the module verifier rejects — the same convention StoreTailValue applies
    // on the way in, applied on the way out.
    if ( IsAggregate( Layout ) )
    {
        return Slot;
    }
    return Builder->CreateLoad( Shape, Slot, Name );
}

llvm::Value *
Volt::Backend::Llvm::LlvmBackend::State::SlotFor ( const Sema::BindingSite &Site, llvm::Type *Shape, std::string_view Name )
{
    if ( const auto It = Frame.Slots.find( Site ); It != Frame.Slots.end() )
    {
        return It->second;
    }

    if ( Frame.Unit != nullptr and Frame.Unit->Scopes != nullptr )
    {
        Sema::ScopeId OwnerScopeId;
        std::visit( Meta::Overloaded{ [this, &OwnerScopeId] ( Frontend::StmtId Stmt )
                                      { OwnerScopeId = Frame.Unit->Scopes->ScopeOf( Stmt ); },
                                      [this, &OwnerScopeId] ( Frontend::ExprId Expr )
                                      { OwnerScopeId = Frame.Unit->Scopes->ScopeOfExpr( Expr ); }, [] ( const auto & ) {} },
                    Site );

        if ( OwnerScopeId.IsValid() and ( Frame.Unit->Scopes->Get( OwnerScopeId ).Kind == Sema::EScopeKind::Unit or
                                          Frame.Unit->Scopes->Get( OwnerScopeId ).Kind == static_cast<Sema::EScopeKind>( 0 ) ) )
        {
            const UnitGlobalKey Key{ .Ordinal = Frame.Unit->Ordinal, .Site = Site };
            if ( const auto GlobIt = ModuleGlobals.find( Key ); GlobIt != ModuleGlobals.end() )
            {
                return GlobIt->second;
            }

            const std::string GlobalName = "_V_global_" + std::to_string( Frame.Unit->Ordinal ) + "_" + std::string( Name );

            auto *GlobalVar = new llvm::GlobalVariable( *Mod, Shape, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
                                                        llvm::Constant::getNullValue( Shape ), GlobalName );
            ModuleGlobals.emplace( Key, GlobalVar );
            return GlobalVar;
        }
    }

    llvm::AllocaInst *Slot = MakeTemp( Shape, Name );
    if ( Slot != nullptr )
    {
        Frame.Slots.emplace( Site, Slot );
    }
    return Slot;
}

void Volt::Backend::Llvm::LlvmBackend::State::EmitStmts ( const Frontend::StmtList &List, bool bTail )
{
    for ( std::size_t Index = 0; Index < List.Size(); ++Index )
    {
        if ( Failed() or Terminated() )
        {
            return;
        }
        EmitStmt( List[Index], bTail and Index + 1 == List.Size() );
    }
}

void Volt::Backend::Llvm::LlvmBackend::State::EmitStmt ( Frontend::StmtId Id, bool bTail )
{
    if ( Failed() or Frame.Unit == nullptr or not Id.IsValid() )
    {
        return;
    }

    const Frontend::AstContext &Ast = *Frame.Unit->Ast;

    std::visit(
        Meta::Overloaded{
            [this, bTail] ( const Frontend::ExprStmt &Node )
            {
                // The tail rule, in one place. Outside result position the
                // value is genuinely discarded — a call for its effect.
                if ( not bTail or not Frame.bReturnsValue )
                {
                    static_cast<void>( EmitExpr( Node.Expr ) );
                    return;
                }

                llvm::Value *Value = EmitExpr( Node.Expr );
                if ( Value == nullptr )
                {
                    return;
                }
                static_cast<void>( Builder->CreateRet( CoerceWidth( Value, Frame.Fn->getReturnType() ) ) );
            },
            [this] ( const Frontend::While &Node )
            {
                // The condition gets its own block rather than being emitted
                // into the entry: `next` branches to it, and it is re-evaluated
                // on every iteration.
                llvm::BasicBlock *Test  = llvm::BasicBlock::Create( Context, "while.cond", Frame.Fn );
                llvm::BasicBlock *Body  = llvm::BasicBlock::Create( Context, "while.body", Frame.Fn );
                llvm::BasicBlock *Merge = llvm::BasicBlock::Create( Context, "while.end", Frame.Fn );

                // A post-test loop — `begin ... end while c` — differs by one
                // edge: entry goes to the body instead of the test, so the
                // body runs once before the condition is ever evaluated. The
                // LoopFrame is unchanged, which is the point of carrying a flag
                // rather than desugaring: `next` still branches to Test.
                static_cast<void>( Builder->CreateBr( Node.bPostTest ? Body : Test ) );
                Builder->SetInsertPoint( Test );
                llvm::Value *Cond = EmitExpr( Node.Cond );
                if ( Cond == nullptr )
                {
                    return;
                }
                static_cast<void>( Builder->CreateCondBr( Cond, Body, Merge ) );

                Frame.Loops.push_back( LoopFrame{ .Latch = Test, .Merge = Merge } );
                Builder->SetInsertPoint( Body );
                EmitStmts( Node.Body, false );
                if ( not Terminated() )
                {
                    static_cast<void>( Builder->CreateBr( Test ) );
                }
                Frame.Loops.pop_back();

                Builder->SetInsertPoint( Merge );
            },
            [this] ( const Frontend::Return &Node )
            {
                if ( not Node.Value.IsValid() )
                {
                    static_cast<void>( Builder->CreateRetVoid() );
                    return;
                }

                llvm::Value *Value = EmitExpr( Node.Value );
                if ( Value == nullptr )
                {
                    return;
                }

                // A `return v` in a function the signature says returns nothing
                // is a middle-end disagreement, not something to silently drop:
                // one of the two read a different signature.
                if ( Frame.Fn->getReturnType()->isVoidTy() )
                {
                    static_cast<void>( Fail( "llvm: `return` with a value in '" + Frame.Fn->getName().str() +
                                             "', whose signature declares no result" ) );
                    return;
                }
                static_cast<void>( Builder->CreateRet( CoerceWidth( Value, Frame.Fn->getReturnType() ) ) );
            },
            [this, Id] ( const Frontend::Break &Node )
            {
                if ( Frame.Loops.empty() )
                {
                    if ( not Frame.bClosure )
                    {
                        static_cast<void>( Fail( "llvm: `break` outside a loop reached codegen" ) );
                        return;
                    }
                    // Inside a closure with no loop of its own, `break` leaves
                    // the *iterating* method — a non-local exit out of a frame
                    // this one does not own. `break <value>` stays refused
                    // (backend phase 6d): there is no result slot anywhere on
                    // this transport to carry it in, the same wall a `while`
                    // hits below, just one call frame further out.
                    if ( Node.Value.IsValid() )
                    {
                        static_cast<void>(
                            Fail( "llvm: `break` with a value at statement " + std::to_string( Id.Value ) +
                                  " leaves a closure body — there is no result slot on the unwind transport to carry it in" ) );
                        return;
                    }
                    static_cast<void>( Builder->CreateStore( Builder->getTrue(), BreakFlagSlot() ) );
                    EmitPoisonedPath();
                    return;
                }
                // `break v` yields a value out of a loop, and a `while` has no
                // value to yield it as.
                if ( Node.Value.IsValid() )
                {
                    static_cast<void>( Fail( "llvm: `break` with a value at statement " + std::to_string( Id.Value ) +
                                             " — a `while` has no value to yield it as" ) );
                    return;
                }
                static_cast<void>( Builder->CreateBr( Frame.Loops.back().Merge ) );
            },
            [this, Id] ( const Frontend::Next &Node )
            {
                // In a closure with no enclosing loop, `next` *is* the block's
                // result: it ends this invocation, which is a `ret`, not a
                // branch. The same word means "continue" only when there is a
                // loop in this frame to continue.
                if ( Frame.Loops.empty() and Frame.bClosure )
                {
                    EmitBlockNext( Node.Value );
                    return;
                }
                if ( Frame.Loops.empty() )
                {
                    static_cast<void>( Fail( "llvm: `next` outside a loop reached codegen" ) );
                    return;
                }
                if ( Node.Value.IsValid() )
                {
                    static_cast<void>( Fail( "llvm: `next` with a value at statement " + std::to_string( Id.Value ) +
                                             " — a `while` iteration has no value to carry" ) );
                    return;
                }
                static_cast<void>( Builder->CreateBr( Frame.Loops.back().Latch ) );
            },
            [this, Id] ( const Frontend::LocalDecl &Node )
            {
                // The local's shape is the one TypeChecker recorded *for the
                // binding site*, not the initialiser's: a declared
                // `x : UInt64 = 0` is storage of the declared width, and the
                // initialiser widens into it.
                const Sema::BindingSite Site{ Id };
                const Sema::LayoutId Shape = LayoutOfValue( *Frame.Values, Frame.Values->SiteType( Site ) );
                llvm::Type *Slot           = TypeOfLayout( Shape );
                if ( Slot == nullptr )
                {
                    static_cast<void>( Fail( "llvm: local '" + std::string( Frame.Unit->Ast->Text( Node.Name ) ) +
                                             "' has no resolved layout" ) );
                    return;
                }

                llvm::Value *Address = SlotFor( Site, Slot, Frame.Unit->Ast->Text( Node.Name ) );
                if ( Address == nullptr or not Node.Init.IsValid() )
                {
                    return;
                }

                llvm::Value *Value = EmitExpr( Node.Init );
                if ( Value == nullptr )
                {
                    return;
                }
                // The initialiser widens into the slot inside EmitStore, which
                // reconciles the two widths for every store there is.
                EmitStore( Address, Value, Shape );
            },
            [this] ( const Frontend::RescueClause & )
            { static_cast<void>( Fail( "llvm: a `rescue` clause needs the exception emitter (backend phase 6)" ) ); },
            [this, Id] ( const auto & )
            {
                static_cast<void>( Fail( "llvm: " + std::string( Frontend::NodeName( Frame.Unit->Ast->Stmt( Id ) ) ) +
                                         " is not a statement this backend emits" ) );
            } },
        Ast.Stmt( Id ) );
}
