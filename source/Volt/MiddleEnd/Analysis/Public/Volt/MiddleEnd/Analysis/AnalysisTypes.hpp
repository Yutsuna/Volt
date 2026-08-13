#pragma once

// The type vocabulary every Analysis translation unit speaks.
//
// Before the Sema -> MiddleEnd split these names all lived in one flat
// namespace, so `SemaTypeId`, `TypeStore`, `PassContext` and `BindingSite`
// resolved unqualified from anywhere in the middle-end. They are four
// different modules now (`TypeSystem`, `Core`, `Resolver`), and re-qualifying
// several hundred uses would have made this migration a rewrite rather than a
// move. The aliases live here instead — declared once, in `Analysis`, and so
// visible by ordinary enclosing-namespace lookup from `Analysis::Raii` and
// `Analysis::Lifetime` too.
//
// This is a naming convenience, not a dependency shortcut: each alias names
// the module that actually owns the type, and that module is a declared
// dependency of Analysis in meson.build.

#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "Volt/MiddleEnd/Resolver/ClosureFrame.hpp"
#include "Volt/MiddleEnd/Resolver/ScopeTable.hpp"
#include "Volt/MiddleEnd/TypeSystem/MemoryLayout.hpp"
#include "Volt/MiddleEnd/TypeSystem/SemaType.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeResolve.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeCompat.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

namespace Volt::MiddleEnd::Analysis
{

using Member          = Volt::MiddleEnd::TypeSystem::Member;
using SemaTypeId      = Volt::MiddleEnd::TypeSystem::SemaTypeId;
using SemaType        = Volt::MiddleEnd::TypeSystem::SemaType;
using UnitTypes       = Volt::MiddleEnd::TypeSystem::UnitTypes;
using UnitSink        = Volt::MiddleEnd::TypeSystem::UnitSink;
using NominalId       = Volt::MiddleEnd::TypeSystem::NominalId;
using NominalType     = Volt::MiddleEnd::TypeSystem::NominalType;
using SigTypeId       = Volt::MiddleEnd::TypeSystem::SigTypeId;
using SigType         = Volt::MiddleEnd::TypeSystem::SigType;
using TypeStore       = Volt::MiddleEnd::TypeSystem::TypeStore;
using EMemberKind     = Volt::MiddleEnd::TypeSystem::EMemberKind;
using LayoutId        = Volt::MiddleEnd::TypeSystem::LayoutId;
using LayoutKind      = Volt::MiddleEnd::TypeSystem::LayoutKind;
using LayoutNode      = Volt::MiddleEnd::TypeSystem::LayoutNode;
using Primitive       = Volt::MiddleEnd::TypeSystem::Primitive;
using Aggregate       = Volt::MiddleEnd::TypeSystem::Aggregate;
using FieldLayout     = Volt::MiddleEnd::TypeSystem::FieldLayout;
using PassContext     = Volt::MiddleEnd::Core::PassContext;
using PassStats       = Volt::MiddleEnd::Core::PassStats;
using BindingSite     = Volt::MiddleEnd::Resolver::BindingSite;
using BindingSiteHash = Volt::MiddleEnd::Resolver::BindingSiteHash;
using ScopeId         = Volt::MiddleEnd::Resolver::ScopeId;
using ScopeTable      = Volt::MiddleEnd::Resolver::ScopeTable;
using Scope           = Volt::MiddleEnd::Resolver::Scope;
using EScopeKind      = Volt::MiddleEnd::Resolver::EScopeKind;
using Binding         = Volt::MiddleEnd::Resolver::Binding;
using Capture         = Volt::MiddleEnd::Resolver::Capture;
using ClosureEnvFrame = Volt::MiddleEnd::Resolver::ClosureEnvFrame;
using ClosureEnvField = Volt::MiddleEnd::Resolver::ClosureEnvField;
using EAssignSite     = Volt::MiddleEnd::TypeSystem::EAssignSite;
using Symbol          = ::Volt::Core::Symbol;

// Declared here so a header can name `Analysis::Lifetime` (or alias it) with
// no include of the Lifetime tree itself — `Lowering/LoweringPasses.hpp` does
// exactly that.
namespace Lifetime
{
} // namespace Lifetime

} // namespace Volt::MiddleEnd::Analysis
