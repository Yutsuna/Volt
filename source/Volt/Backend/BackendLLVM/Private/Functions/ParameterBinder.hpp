#pragma once

// ParameterBinder.hpp — giving an incoming argument its storage.
//
// One way for every parameter list there is — a method's, a monomorphised
// body's, a synthesized closure's — because `bByAddress` must be the *same*
// answer ParamTypeOfLayout gave when it built the signature. Two call sites
// deciding that independently is how a pointer parameter becomes its own slot
// and every read of it dereferences one level too far.

#include "Volt/BackendCore/BackendInput.hpp"

#include "Core/LlvmFwd.hpp"

#include <string_view>

namespace Volt
{

namespace Backend
{

    namespace Llvm
    {

        class BodyEmitter;

        // `bByAddress`: an aggregate travels as a pointer to the caller's
        // storage and therefore already *is* its slot; everything else arrives as
        // a bare value and needs one. False when the slot could not be opened.
        [[nodiscard]] bool BindParameter ( BodyEmitter &Emitter,
                                           const Sema::BindingSite &Site,
                                           llvm::Value *Arg,
                                           bool bByAddress,
                                           std::string_view Name );

        // `def initialize( @x : T )`: bind the parameter *and* store it into the
        // field of that name. No-op for an ordinary parameter.
        void BindInstanceVarParam ( BodyEmitter &Emitter, Frontend::ParamId ParamRef, llvm::Value *Value );

    } // namespace Llvm

} // namespace Backend

} // namespace Volt
