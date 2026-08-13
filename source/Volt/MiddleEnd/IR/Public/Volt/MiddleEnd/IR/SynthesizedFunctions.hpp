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

        private:

            std::vector<SynthesizedFunction> Entries;
        };

    } // namespace IR

} // namespace MiddleEnd

} // namespace Volt
