// ExprOperatorEmitter.cpp — the operator protocol, in the one order that keeps
// a backend free of member lookup.
//
// `Binary` and `Unary` are core nodes but mean **two different things**, and the
// discriminator is never a type name (rules/core-ast.md):
//
//   CalleeResolution has this node, Decl != nullptr  ->  a call to that method
//   otherwise (primitive/pointer receiver)           ->  a machine instruction
//
// MemberType records that resolution for `Binary`/`Unary` exactly as it does
// for `Member`, so the decision has already been taken upstream. What lives
// here is the shared half of reading it back: both operator emitters ask the
// same question in the same order, and answering it differently in the two
// files is precisely the drift this function exists to prevent.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"

const Volt::Sema::CalleeEntry *Volt::Backend::Llvm::ResolvedOperator ( BodyEmitter &Emitter, Frontend::ExprId Id )
{
    // `bAbstract` is what excludes the primitive contracts: `Arithmetic#+` is
    // declared so that `a + b` has a *type*, and a Primitive/Pointer receiver is
    // exempt from providing a body because the backend supplies the instruction
    // (rules/zero-hardcode.md). A resolution naming one of those is therefore
    // not a call — it is the signal to select an opcode instead.
    const Sema::CalleeEntry *Entry = Emitter.Frame().Callees->Get( Id );
    if ( Entry != nullptr and Entry->Decl != nullptr and not Entry->Decl->bAbstract )
    {
        return Entry;
    }
    return nullptr;
}

Volt::Backend::Llvm::EOpFamily Volt::Backend::Llvm::FamilyOfExpr ( BodyEmitter &Emitter, Frontend::ExprId Id )
{
    return FamilyOf( Emitter.Types().SpellingOf( Emitter.LayoutOfExpr( Id ) ) );
}
