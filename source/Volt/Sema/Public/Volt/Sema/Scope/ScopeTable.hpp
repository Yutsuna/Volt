#pragma once
#include "Volt/MiddleEnd/Resolver/ScopeTable.hpp"
namespace Volt::Sema
{
using ::Volt::Core::Symbol;
using ScopeTag        = MiddleEnd::Resolver::ScopeTag;
using ScopeId         = MiddleEnd::Resolver::ScopeId;
using EScopeKind      = MiddleEnd::Resolver::EScopeKind;
using BindingSite     = MiddleEnd::Resolver::BindingSite;
using BindingSiteHash = MiddleEnd::Resolver::BindingSiteHash;
using MiddleEnd::Resolver::IsValueBinding;
using Scope      = MiddleEnd::Resolver::Scope;
using Binding    = MiddleEnd::Resolver::Binding;
using Capture    = MiddleEnd::Resolver::Capture;
using ScopeTable = MiddleEnd::Resolver::ScopeTable;
} // namespace Volt::Sema
