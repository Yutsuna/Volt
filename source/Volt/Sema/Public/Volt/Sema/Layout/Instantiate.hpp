#pragma once
#include "Volt/MiddleEnd/IR/ExprRedirect.hpp"
#include "Volt/MiddleEnd/TypeSystem/Instantiate.hpp"
namespace Volt::Sema
{
using InstantiatedBody = MiddleEnd::TypeSystem::InstantiatedBody;
using MiddleEnd::TypeSystem::ReinstantiateBody;
using ExprRedirectMap = MiddleEnd::IR::ExprRedirectMap;
} // namespace Volt::Sema
