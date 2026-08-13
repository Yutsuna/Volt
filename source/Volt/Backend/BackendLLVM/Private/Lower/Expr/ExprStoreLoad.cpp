// ExprStoreLoad.cpp — moving a value into storage.
//
// One function, and the width reconciliation it centralises is the point of it:
// there are four call sites that store (a LocalDecl's initialiser, an Assign, a
// converging tail value, an `@x`-declaring parameter), and only one of them
// used to coerce. So a mismatched width became silent corruption — `result = 1`
// typed Int32 against an Int8 local put a `store i32` into an `alloca i8`,
// which smashes the frame. Reconciling here means every store there is passes
// through the same rule.

#include "Lower/BodyEmitter.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

namespace
{

// The width a store destination actually has, when the address itself records
// it. Opaque pointers carry no pointee, but the two shapes a slot ever takes
// do: an `alloca` in the entry block, or a module global for a unit-scope
// binding. Anything else — a GEP into an aggregate — answers nothing, and the
// caller falls back on the layout it was handed.
[[nodiscard]] llvm::Type *SlotTypeOf ( const llvm::Value *Address )
{
    if ( const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>( Address ) )
    {
        return Slot->getAllocatedType();
    }
    if ( const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>( Address ) )
    {
        return Global->getValueType();
    }
    return nullptr;
}

} // namespace

void Volt::Backend::Llvm::BodyEmitter::EmitStore ( llvm::Value *Address,
                                                   llvm::Value *Value,
                                                   MiddleEnd::TypeSystem::LayoutId Shape )
{
    if ( Address == nullptr or Value == nullptr or Failed() )
    {
        return;
    }

    if ( not IsAggregate( Shape ) )
    {
        // The destination's *own* width is the authority, and an alloca or a
        // global carries it. `Shape` is the assignment target's **expression**
        // type, which can lag behind the binding site that minted the slot:
        // ConstrainNode( Identifier ) moves a local's site type without
        // revisiting the uses already inferred against the old one, so
        // `i = 0_u64` leaves those uses reading Int32 while the slot is
        // correctly i64 — trusting Shape there would reject a store that is
        // right. Shape is the fallback for an address carrying no type of its
        // own, a GEP into an aggregate, where it is the field's own layout.
        llvm::Type *Slot = SlotTypeOf( Address );
        if ( Slot == nullptr )
        {
            Slot = Types().TypeOfLayout( Shape );
        }
        Value = CoerceWidth( Value, Slot );
        if ( Slot != nullptr and Value->getType() != Slot )
        {
            // CoerceWidth widens and never narrows, by design: Sema's
            // IsWideningScalar admits an i8 where a u64 is wanted and refuses
            // the reverse (rules/zero-hardcode.md). So a value still too wide
            // here means the middle-end handed out two different answers for one
            // binding — Values.ExprType( value ) against Values.SiteType( slot )
            // — and the honest reading is a hole in those tables, not a
            // truncation for this emitter to invent.
            std::string Report;
            llvm::raw_string_ostream Out{ Report };
            Out << "llvm: cannot store a value of type ";
            Value->getType()->print( Out );
            Out << " into a slot of type ";
            Slot->print( Out );
            Out << " — the middle-end typed this value and its binding site differently";
            static_cast<void>( Fail( std::move( Report ) ) );
            return;
        }
        static_cast<void>( Ctx().Builder().CreateStore( Value, Address ) );
        return;
    }

    // An aggregate is only ever an address, so assigning one is a copy — and its
    // size is LayoutEngine's, the single ABI authority, not sizeof-anything the
    // emitter recomputes.
    const SizeAlign Measure =
        ( *Services().Layouts )
            ->Of( Shape ); // NOLINT(bugprone-unchecked-optional-access) — Layouts is always emplaced before any emission
    static_cast<void>( Ctx().Builder().CreateMemCpy( Address, llvm::MaybeAlign( Measure.Alignment ), Value,
                                                     llvm::MaybeAlign( Measure.Alignment ),
                                                     Ctx().Builder().getInt64( Measure.Size ) ) );
}
