// StmtLocalDeclEmitter.cpp — `x : T = init`, and the slot machinery behind it.
//
// `SlotFor` lives here rather than in BodyEmitter.cpp because this is the node
// that mints most slots, and because the one non-obvious thing it does — a
// unit-scope binding becomes a module global rather than an alloca — is a
// property of what a local *is*, not of how a body is emitted.

#include "Lower/Stmt/StmtEmitter.hpp"

#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Core/ModuleLocal.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"
#include "Volt/BackendCore/DiagnosticSink.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <string>
#include <variant>

llvm::Value *Volt::Backend::Llvm::BodyEmitter::SlotFor ( const MiddleEnd::Resolver::BindingSite &Site,
                                                         llvm::Type *Shape,
                                                         std::string_view Name )
{
    FunctionFrame &Local = Frame();

    if ( const auto It = Local.Slots.find( Site ); It != Local.Slots.end() )
    {
        return It->second;
    }

    if ( Local.Unit != nullptr and Local.Unit->Scopes != nullptr )
    {
        MiddleEnd::Resolver::ScopeId OwnerScopeId;
        std::visit( Meta::Overloaded{ [&Local, &OwnerScopeId] ( Frontend::StmtId Stmt )
                                      { OwnerScopeId = Local.Unit->Scopes->ScopeOf( Stmt ); },
                                      [&Local, &OwnerScopeId] ( Frontend::ExprId Expr )
                                      { OwnerScopeId = Local.Unit->Scopes->ScopeOfExpr( Expr ); }, [] ( const auto & ) {} },
                    Site );

        if ( OwnerScopeId.IsValid() and
             ( Local.Unit->Scopes->Get( OwnerScopeId ).Kind == MiddleEnd::Resolver::EScopeKind::Unit or
               Local.Unit->Scopes->Get( OwnerScopeId ).Kind == static_cast<MiddleEnd::Resolver::EScopeKind>( 0 ) ) )
        {
            const UnitGlobalKey Key{ .Ordinal = Local.Unit->Ordinal, .Site = Site };
            if ( const auto GlobIt = Services().ModuleGlobals->find( Key ); GlobIt != Services().ModuleGlobals->end() )
            {
                // Translated, not returned as found: split across modules, this
                // binding is one piece of storage that several of them read, and
                // a per-module copy would silently be a *second* variable.
                return LocalCopy( Services(), GlobIt->second );
            }

            const std::string GlobalName = "_V_global_" + std::to_string( Local.Unit->Ordinal ) + "_" + std::string( Name );

            // Internal while the build is one module — nothing outside it has a
            // name for this. PerUnit gives every other module a name for it, so
            // it has to be one the JIT can resolve.
            const llvm::GlobalValue::LinkageTypes Linkage =
                PerUnitModules( Services() ) ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage;

            auto *GlobalVar = new llvm::GlobalVariable( Ctx().Mod(), Shape, /*isConstant=*/false, Linkage,
                                                        llvm::Constant::getNullValue( Shape ), GlobalName );
            Services().ModuleGlobals->emplace( Key, GlobalVar );
            return GlobalVar;
        }
    }

    llvm::AllocaInst *Slot = MakeTemp( Shape, Name );
    if ( Slot != nullptr )
    {
        Local.Slots.emplace( Site, Slot );
    }
    return Slot;
}

void Volt::Backend::Llvm::EmitLocalDecl ( BodyEmitter &Emitter, Frontend::StmtId Id, const Frontend::LocalDecl &Node )
{
    FunctionFrame &Frame = Emitter.Frame();

    // The local's shape is the one TypeChecker recorded *for the binding site*,
    // not the initialiser's: a declared `x : UInt64 = 0` is storage of the
    // declared width, and the initialiser widens into it.
    const MiddleEnd::Resolver::BindingSite Site{ Id };
    const MiddleEnd::TypeSystem::LayoutId Shape = Emitter.Types().LayoutOfValue( *Frame.Values, Frame.Values->SiteType( Site ) );
    llvm::Type *Slot                            = Emitter.Types().TypeOfLayout( Shape );
    if ( Slot == nullptr )
    {
        static_cast<void>(
            Emitter.Fail( "llvm: local '" + std::string( Frame.Unit->Ast->Text( Node.Name ) ) + "' has no resolved layout" ) );
        return;
    }

    llvm::Value *Address = Emitter.SlotFor( Site, Slot, Frame.Unit->Ast->Text( Node.Name ) );
    if ( Address == nullptr or not Node.Init.IsValid() )
    {
        return;
    }

    llvm::Value *Value = Emitter.EmitExpr( Node.Init );
    if ( Value == nullptr )
    {
        return;
    }
    // The initialiser widens into the slot inside EmitStore, which reconciles
    // the two widths for every store there is.
    Emitter.EmitStore( Address, Value, Shape );
}
