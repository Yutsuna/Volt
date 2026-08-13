#pragma once
#include "Volt/MiddleEnd/TypeSystem/TypeResolve.hpp"
namespace Volt::Sema
{
using SigSink        = MiddleEnd::TypeSystem::SigSink;
using UnitSink       = MiddleEnd::TypeSystem::UnitSink;
using BoundViolation = MiddleEnd::TypeSystem::BoundViolation;
using MiddleEnd::TypeSystem::CheckGenericBounds;
using MiddleEnd::TypeSystem::Instantiate;
using MiddleEnd::TypeSystem::ResolveTypeExpr;
using InstantiatedMember = MiddleEnd::TypeSystem::InstantiatedMember;
using MiddleEnd::TypeSystem::IsSubclassOf;
using MiddleEnd::TypeSystem::LookupMemberOn;
using MiddleEnd::TypeSystem::UnifySig;
} // namespace Volt::Sema
