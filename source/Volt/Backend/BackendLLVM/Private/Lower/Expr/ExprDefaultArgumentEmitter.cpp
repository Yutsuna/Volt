// ExprDefaultArgumentEmitter.cpp — a parameter the call omitted.
//
// The default expression belongs to the *declaring* unit: its type, and any
// callee it resolves, live in that unit's tables and nowhere else. So the frame
// switches views for exactly the span of this one emission, and switches back —
// only those three fields move. The slots, `self` and the block stay this
// frame's, because a default that reached for one of them would be reaching into
// a frame that does not exist yet, and the loud failure that follows is the
// right one.

#include "Lower/BodyEmitter.hpp"

#include "Core/EmitterServices.hpp"
#include "Lower/FunctionFrame.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <cstddef>
#include <variant>

llvm::Value *Volt::Backend::Llvm::BodyEmitter::EmitDefaultArgument ( const MiddleEnd::TypeSystem::Member &Decl,
                                                                     std::size_t Index )
{
    const UnitView *Home = nullptr;
    for ( const UnitView &View : Services().Build->Units )
    {
        if ( View.Ordinal == Decl.Unit and View.Ast != nullptr )
        {
            Home = &View;
            break;
        }
    }
    if ( Home == nullptr )
    {
        return nullptr;
    }

    const auto *Method = std::get_if<Frontend::Method>( &Home->Ast->Decl( Decl.Decl ) );
    if ( Method == nullptr or Index >= Method->Params.Size() )
    {
        return nullptr;
    }
    const Frontend::ExprId Default = Home->Ast->GetParam( Method->Params[Index] ).Default;
    if ( not Default.IsValid() )
    {
        return nullptr;
    }

    FunctionFrame &Fr = Frame();

    const UnitView *SavedUnit                           = Fr.Unit;
    const MiddleEnd::TypeSystem::UnitTypes *SavedValues = Fr.Values;
    const MiddleEnd::IR::UnitCallees *SavedCallees      = Fr.Callees;
    Fr.Unit                                             = Home;
    Fr.Values                                           = Home->Values;
    Fr.Callees                                          = Home->Callees;

    llvm::Value *Value = EmitExpr( Default );

    Fr.Unit    = SavedUnit;
    Fr.Values  = SavedValues;
    Fr.Callees = SavedCallees;
    return Value;
}
