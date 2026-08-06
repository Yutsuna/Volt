// ParameterBinder.cpp — see ParameterBinder.hpp.

#include "Functions/ParameterBinder.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/IRBuilder.h>

#include <string>

bool Volt::Backend::Llvm::BindParameter (
    BodyEmitter &Emitter, const Sema::BindingSite &Site, llvm::Value *Arg, bool bByAddress, std::string_view Name )
{
    // An aggregate (and a `&block`, whose `{ code, env }` pair is one) arrives as
    // a pointer to the caller's storage and *is* its own slot, so it is kept
    // as-is. A scalar arrives as a bare value with no backing storage — without
    // an alloca here, a later read of it as an Identifier (LoadPlace ->
    // EmitAddress -> CreateLoad) loads *through* the value as if it pointed at
    // itself.
    //
    // The question is the parameter's **layout**, never its LLVM type. `ptr` is
    // not a proxy for "aggregate": a `@[Primitive( "ptr", 64 )]` scalar — every
    // `Pointer<T>` — maps to `ptr` too, and answering from the LLVM type made
    // every pointer parameter its own slot, so `String.from_c_string( p )` read
    // `*p` instead of `p` and handed `strlen` whatever the pointee held.
    if ( bByAddress )
    {
        Emitter.Frame().Slots.emplace( Site, Arg );
        return true;
    }

    llvm::Value *Slot = Emitter.SlotFor( Site, Arg->getType(), Name );
    if ( Slot == nullptr )
    {
        return false;
    }
    static_cast<void>( Emitter.Ctx().Builder().CreateStore( Arg, Slot ) );
    return true;
}

void Volt::Backend::Llvm::BindInstanceVarParam ( BodyEmitter &Emitter, Frontend::ParamId ParamRef, llvm::Value *Value )
{
    FunctionFrame &Frame            = Emitter.Frame();
    const Frontend::Param &Declared = Frame.Unit->Ast->GetParam( ParamRef );

    // `def initialize( @x : Int32 )` declares a parameter *and* stores it into
    // the field of that name — the parser records which (`Param::bInstanceVar`,
    // with the sigil already stripped) and no pass materialises the store, so it
    // is emitted here as part of binding the parameter. Without it every field so
    // declared stays whatever the frame happened to hold, silently: the stdlib's
    // `Exception#initialize( @message : String )` and any `Point.new( 3, 4 )`
    // both depend on it.
    if ( not Declared.bInstanceVar or Value == nullptr )
    {
        return;
    }
    if ( Frame.Self == nullptr )
    {
        static_cast<void>( Emitter.Fail( "llvm: '@" + std::string( Frame.Unit->Ast->Text( Declared.Name ) ) +
                                         "' is a field-assigning parameter of a method with no receiver" ) );
        return;
    }

    llvm::Value *Address =
        Emitter.FieldAddress( Frame.Self, Frame.SelfLayout, Frame.Unit->Ast->Text( Declared.Name ), Frontend::ExprId{} );
    if ( Address == nullptr )
    {
        return;
    }
    Emitter.EmitStore( Address, Value,
                       Emitter.Types().LayoutOfValue( *Frame.Values, Frame.Values->SiteType( Sema::BindingSite{ ParamRef } ) ) );
}
