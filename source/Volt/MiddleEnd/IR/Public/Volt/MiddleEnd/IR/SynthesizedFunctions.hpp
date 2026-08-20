#pragma once

#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"
#include "VoltMiddleEndIR_export.hpp"

#include <span>
#include <vector>

namespace Volt
{

namespace Meta
{
    class Writer;
    class Reader;
} // namespace Meta

namespace MiddleEnd
{

    namespace TypeSystem
    {
        using SemaTypeId = Volt::MiddleEnd::TypeSystem::SemaTypeId;
    } // namespace TypeSystem

    namespace IR
    {

        struct SynthesizedFunction
        {
            Frontend::DeclId Decl;
            TypeSystem::SemaTypeId Result;
            ::Volt::Core::SmallVec<TypeSystem::SemaTypeId, 4> Params;
        };

        class VOLT_MIDDLEEND_IR_EXPORT SynthesizedFunctions
        {

        public:

            void Add ( SynthesizedFunction Fn )
            {
                Entries.push_back( std::move( Fn ) );
            }

            [[nodiscard]] std::span<const SynthesizedFunction> All () const
            {
                return Entries;
            }

            // Round-tripped by the stdlib frontend cache, exactly like
            // UnitCallees. Not optional: the AST a cache hit restores still
            // holds the `FuncAddr` nodes ClosureLifting produced, and this
            // table is the *only* place their target Decl is registered
            // (BackendLLVM's EmitFuncAddr looks nowhere else). Without it a
            // cache hit resurrects every lifted stdlib closure as a dangling
            // address — which is a codegen failure, not a cache miss.
            void SerializeCache ( Meta::Writer &W ) const;
            [[nodiscard]] bool DeserializeCache ( Meta::Reader &R );

        private:

            std::vector<SynthesizedFunction> Entries;
        };

    } // namespace IR

} // namespace MiddleEnd

} // namespace Volt
