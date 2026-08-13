#pragma once
#include "Volt/MiddleEnd/TypeSystem/MemoryLayout.hpp"
namespace Volt::Sema
{
using LayoutTag   = MiddleEnd::TypeSystem::LayoutTag;
using LayoutId    = MiddleEnd::TypeSystem::LayoutId;
using Primitive   = MiddleEnd::TypeSystem::Primitive;
using Pointer     = MiddleEnd::TypeSystem::Pointer;
using FieldLayout = MiddleEnd::TypeSystem::FieldLayout;
using Aggregate   = MiddleEnd::TypeSystem::Aggregate;
using LayoutNode  = MiddleEnd::TypeSystem::LayoutNode;
using LayoutKind  = MiddleEnd::TypeSystem::LayoutKind;
using MiddleEnd::TypeSystem::KindOf;
} // namespace Volt::Sema
