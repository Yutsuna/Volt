// ExprPlaceEmitter.cpp — the *address* of an assignable expression.
//
// `EmitAddress` is the second std::visit site of the expression category, and
// deliberately a much smaller one: only four node kinds are places (`x`, `@x`,
// `o.x`, `*p`), and anything else reaching it is a middle-end contract
// violation reported by name. `LoadPlace` is the address plus the load, which
// is what every one of those kinds means in *value* position.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <string>
#include <variant>

llvm::Value *Volt::Backend::Llvm::BodyEmitter::EmitAddress ( Frontend::ExprId Id )
{
    if ( Failed() or Frame().Unit == nullptr or not Id.IsValid() )
    {
        return nullptr;
    }

    // Same substitution EmitExpr applies, and for the same reason (its own
    // comment): a monomorphised body may read a captured variable's use site
    // as a *place* too (`block.call( item )`'s receiver), and that site was
    // redirected to a Deref chain, not mutated in place.
    if ( Frame().Redirects != nullptr )
    {
        if ( const auto It = Frame().Redirects->find( Id.Value ); It != Frame().Redirects->end() )
        {
            Id = It->second;
        }
    }

    const Frontend::AstContext &Ast = *Frame().Unit->Ast;

    return std::visit(
        Meta::Overloaded{
            [this, Id] ( const Frontend::Identifier &Node ) -> llvm::Value *
            {
                // The binding is ScopeResolver's answer, never a re-resolution:
                // a name the resolver did not bind is not a name this emitter
                // may go looking for.
                const Sema::Binding *Bound = Frame().Unit->Scopes->BindingOf( Id );
                if ( Bound == nullptr )
                {
                    static_cast<void>( Fail( "llvm: identifier '" + std::string( Frame().Unit->Ast->Text( Node.Name ) ) +
                                             "' reached codegen with no scope binding" ) );
                    return nullptr;
                }

                if ( const auto It = Frame().Slots.find( Bound->Site ); It != Frame().Slots.end() )
                {
                    return It->second;
                }

                if ( Bound->Owner.IsValid() and
                     ( Frame().Unit->Scopes->Get( Bound->Owner ).Kind == Sema::EScopeKind::Unit or
                       Frame().Unit->Scopes->Get( Bound->Owner ).Kind == static_cast<Sema::EScopeKind>( 0 ) ) )
                {
                    const Sema::LayoutId Shape =
                        Types().LayoutOfValue( *Frame().Values, Frame().Values->SiteType( Bound->Site ) );
                    llvm::Type *Slot = Types().TypeOfLayout( Shape );
                    if ( Slot == nullptr )
                    {
                        static_cast<void>( Fail( "llvm: unit global '" + std::string( Frame().Unit->Ast->Text( Bound->Name ) ) +
                                                 "' has no resolved layout" ) );
                        return nullptr;
                    }
                    return SlotFor( Bound->Site, Slot, Frame().Unit->Ast->Text( Bound->Name ) );
                }

                // An implicit local (`buf = expr`, no `: Type`) has no declaring
                // *statement* to open its storage the way a LocalDecl does, so
                // the occurrence ScopeResolver recorded as its site — the Assign
                // target, and no other — opens it here. Everything else about it
                // is a local like any other: the shape is the one TypeChecker
                // recorded for the site.
                if ( const auto *Site = std::get_if<Frontend::ExprId>( &Bound->Site ); Site != nullptr )
                {
                    const Sema::LayoutId Shape =
                        Types().LayoutOfValue( *Frame().Values, Frame().Values->SiteType( Bound->Site ) );
                    llvm::Type *Slot = Types().TypeOfLayout( Shape );
                    if ( Slot == nullptr )
                    {
                        static_cast<void>( Fail( "llvm: local '" + std::string( Frame().Unit->Ast->Text( Bound->Name ) ) +
                                                 "' has no resolved layout" ) );
                        return nullptr;
                    }
                    return SlotFor( Bound->Site, Slot, Frame().Unit->Ast->Text( Bound->Name ) );
                }

                // A local declared in a branch the walk has not reached is
                // impossible, and a captured one was bound from the environment
                // on entry — so what is left is a name ScopeResolver bound
                // outside this body without recording it as a capture.
                static_cast<void>( Fail( "llvm: '" + std::string( Frame().Unit->Ast->Text( Bound->Name ) ) +
                                         "' has no slot in this function, and no capture of it was recorded" ) );
                return nullptr;
            },
            [this, Id] ( const Frontend::InstanceVar &Node ) -> llvm::Value *
            {
                // The written spelling keeps its `@` (the parser interns the
                // token whole); a field is declared without one. Sema strips it
                // in exactly one place too — LookupOn's CleanName — and the two
                // must agree, or a member resolves and its storage does not.
                const std::string_view Field = FieldNameOf( Frame().Unit->Ast->Text( Node.Name ) );
                if ( Frame().Self == nullptr )
                {
                    static_cast<void>( Fail( MissingSelf( "'@" + std::string( Field ) + "'" ) ) );
                    return nullptr;
                }
                return FieldAddress( Frame().Self, Frame().SelfLayout, Field, Id );
            },
            [this, Id] ( const Frontend::Member &Node ) -> llvm::Value *
            {
                llvm::Value *Object = EmitExpr( Node.Object );
                if ( Object == nullptr )
                {
                    return nullptr;
                }
                return FieldAddress( Object, LayoutOfExpr( Node.Object ), Frame().Unit->Ast->Text( Node.Name ), Id );
            },
            // `*p = v`: the place *is* the pointer the operand evaluates to.
            [this] ( const Frontend::Deref &Node ) -> llvm::Value * { return EmitExpr( Node.Operand ); },
            [this, Id] ( const auto & ) -> llvm::Value *
            {
                static_cast<void>( Fail( "llvm: " + std::string( Frontend::NodeName( Frame().Unit->Ast->Expr( Id ) ) ) +
                                         " is not an assignable place" ) );
                return nullptr;
            } },
        Ast.Expr( Id ) );
}

std::string_view Volt::Backend::Llvm::BodyEmitter::FieldNameOf ( std::string_view Written )
{
    return Written.starts_with( '@' ) ? Written.substr( 1 ) : Written;
}

llvm::Value *Volt::Backend::Llvm::BodyEmitter::FieldAddress ( llvm::Value *Object,
                                                              Sema::LayoutId Shape,
                                                              std::string_view Name,
                                                              Frontend::ExprId Id )
{
    llvm::Type *Struct = Types().TypeOfLayout( Shape );
    if ( Struct == nullptr or not Struct->isStructTy() )
    {
        static_cast<void>( Fail( "llvm: field '" + std::string( Name ) + "' on a receiver with no aggregate layout" ) );
        return nullptr;
    }

    // The index is the field's position in the *layout*, which is declaration
    // order (abi.md) — the same order TypeMapper built the LLVM struct in, and
    // the same one AbiVerifier already cross-checked against LayoutEngine. So a
    // struct GEP and LayoutEngine::FieldOffset name the same byte by
    // construction.
    const Sema::Aggregate &Fields = std::get<Sema::Aggregate>( Services().Build->Types->Get( Shape ) );
    for ( std::size_t Index = 0; Index < Fields.Fields.Size(); ++Index )
    {
        if ( Services().Build->Types->Text( Fields.Fields[Index].Name ) == Name )
        {
            return Ctx().Builder().CreateStructGEP( Struct, Object, static_cast<unsigned>( Index ),
                                                    "field." + std::string( Name ) );
        }
    }

    static_cast<void>(
        Fail( "llvm: no field '" + std::string( Name ) + "' in the layout of expression " + std::to_string( Id.Value ) ) );
    return nullptr;
}

llvm::Value *Volt::Backend::Llvm::BodyEmitter::LoadPlace ( Frontend::ExprId Id )
{
    llvm::Value *Address = EmitAddress( Id );
    if ( Address == nullptr )
    {
        return nullptr;
    }

    const Sema::LayoutId Shape = LayoutOfExpr( Id );
    // An aggregate never leaves its storage: its value *is* the address.
    if ( IsAggregate( Shape ) )
    {
        return Address;
    }

    llvm::Type *Loaded = Types().TypeOfLayout( Shape );
    if ( Loaded == nullptr )
    {
        static_cast<void>( Fail( "llvm: expression " + std::to_string( Id.Value ) + " has no resolved layout to load" ) );
        return nullptr;
    }
    return Ctx().Builder().CreateLoad( Loaded, Address );
}
