#pragma once
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"
// The pre-migration header transitively pulled in TypeStore.hpp (a SemaType
// names a NominalId, and its store); mirror that here through the sibling
// compat shim so Volt::Sema::TypeStore / NominalId / Member / EMemberKind /
// SigTypeId stay reachable for callers that only ever included SemaType.hpp.
#include "Volt/Sema/Layout/TypeStore.hpp"
namespace Volt::Sema
{
using SemaTypeTag = MiddleEnd::TypeSystem::SemaTypeTag;
using SemaTypeId  = MiddleEnd::TypeSystem::SemaTypeId;
using SemaType    = MiddleEnd::TypeSystem::SemaType;
using UnitTypes   = MiddleEnd::TypeSystem::UnitTypes;
} // namespace Volt::Sema
