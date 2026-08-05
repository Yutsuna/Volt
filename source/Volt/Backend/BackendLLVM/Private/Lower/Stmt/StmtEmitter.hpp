#pragma once

// StmtEmitter.hpp — the statement category's dispatch, and the named arms it
// dispatches to.
//
// Volt has no `return` requirement: `Int8#<=>` ends in an if/elsif/else chain
// and its value is the value of whichever branch ran. That is a *structural*
// property, not a typing one, so it is decided by position alone:
//
//   EmitStmts( List, bTail ) marks the last statement of a list as being in
//   result position, and exactly one node kind does anything with it — an
//   ExprStmt emits `ret`.
//
// No other node propagates it, because no other node's value can be the
// function's: a `while` has none, and a `return` already is one. `If` used to be
// the second reader, forwarding the flag into both branches; now that it is an
// expression it converges its branches into a slot and hands the loaded value
// back to the enclosing ExprStmt, which emits the single `ret`.
//
// One `std::visit` for the whole category (in StmtEmitter.cpp), like the
// expression side: a statement the contract says cannot reach a backend falls
// into the `auto` arm and is reported by name. The arms themselves are the
// named functions below rather than inline lambdas — the visit site stays a
// table of one-liners, and each arm is separately readable and separately
// recompiled.

#include "Volt/BackendCore/BackendInput.hpp"

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class BodyEmitter;

        // The tail rule, in one place. Outside result position the value is
        // genuinely discarded — a call for its effect.
        void EmitExprStmt ( BodyEmitter &Emitter, const Frontend::ExprStmt &Node, bool bTail );

        // StmtLoopEmitter.cpp
        void EmitWhile ( BodyEmitter &Emitter, const Frontend::While &Node );

        // StmtReturnBreakNext.cpp
        void EmitReturn ( BodyEmitter &Emitter, const Frontend::Return &Node );
        void EmitBreak ( BodyEmitter &Emitter, Frontend::StmtId Id, const Frontend::Break &Node );
        void EmitNext ( BodyEmitter &Emitter, Frontend::StmtId Id, const Frontend::Next &Node );

        // StmtLocalDeclEmitter.cpp
        void EmitLocalDecl ( BodyEmitter &Emitter, Frontend::StmtId Id, const Frontend::LocalDecl &Node );

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
