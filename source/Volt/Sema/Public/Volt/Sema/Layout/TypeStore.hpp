#pragma once
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"
#include "Volt/Sema/Layout/MemoryLayout.hpp"
namespace Volt::Sema
{
using ::Volt::Core::Symbol;
using NominalTag  = MiddleEnd::TypeSystem::NominalTag;
using NominalId   = MiddleEnd::TypeSystem::NominalId;
using SigTypeTag  = MiddleEnd::TypeSystem::SigTypeTag;
using SigTypeId   = MiddleEnd::TypeSystem::SigTypeId;
using SigType     = MiddleEnd::TypeSystem::SigType;
using EMemberKind = MiddleEnd::TypeSystem::EMemberKind;
using Member      = MiddleEnd::TypeSystem::Member;
using NominalType = MiddleEnd::TypeSystem::NominalType;
using TypeStore   = MiddleEnd::TypeSystem::TypeStore;
} // namespace Volt::Sema
