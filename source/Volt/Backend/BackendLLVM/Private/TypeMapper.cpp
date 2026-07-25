// TypeMapper.cpp — LayoutId -> llvm::Type*, and the ABI cross-check.
//
// The mapping reads a layout and nothing else: Primitive{ Spelling, Bits },
// Pointer, Aggregate{ Fields }. `"i32"` and `"f64"` are opaque interned
// strings that select a *machine* shape; the compiler never learns which Volt
// type wrote them (rules/zero-hardcode.md). Signedness is deliberately absent
// here — it lives in the instruction, not in the type.

#include "LlvmState.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Type.h>

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace
{

// The float family is identified by the spelling's first character, the same
// one-character test `rules/zero-hardcode.md` grants the compiler. Width then
// picks the IEEE format; anything else is a middle-end fact the emitter has no
// machine type for, and is reported rather than rounded to the nearest.
[[nodiscard]] llvm::Type *FloatTypeOf ( llvm::LLVMContext &Context, std::uint32_t Bits )
{
    switch ( Bits )
    {
    case 16:
        return llvm::Type::getHalfTy( Context );
    case 32:
        return llvm::Type::getFloatTy( Context );
    case 64:
        return llvm::Type::getDoubleTy( Context );
    case 128:
        return llvm::Type::getFP128Ty( Context );
    default:
        return nullptr;
    }
}

} // namespace

llvm::Type *Volt::Backend::Llvm::LlvmBackend::State::TypeOfLayout ( Sema::LayoutId Id )
{
    if ( not Id.IsValid() or Build == nullptr or Build->Types == nullptr )
    {
        return nullptr;
    }

    if ( const auto It = TypeCache.find( Id.Value ); It != TypeCache.end() )
    {
        return It->second;
    }

    const Sema::TypeStore &Store = *Build->Types;

    llvm::Type *Result = std::visit(
        Meta::Overloaded{
            [] ( const std::monostate & ) -> llvm::Type * { return nullptr; },
            [this, &Store] ( const Sema::Primitive &Node ) -> llvm::Type *
            {
                const std::string_view Spelling = Node.Spelling.IsValid() ? Store.Text( Node.Spelling ) : std::string_view{};
                if ( Spelling.empty() )
                {
                    static_cast<void>( Fail( "llvm: a primitive layout carries no spelling" ) );
                    return nullptr;
                }

                // Opaque pointers: a pointer is one machine word whatever it
                // points at, and the pointee only ever informs a GEP.
                if ( Spelling == "ptr" )
                {
                    return llvm::PointerType::get( Context, 0 );
                }

                if ( Spelling.front() == 'f' )
                {
                    llvm::Type *Float = FloatTypeOf( Context, Node.Bits );
                    if ( Float == nullptr )
                    {
                        static_cast<void>( Fail( "llvm: no machine float type for '" + std::string( Spelling ) + "' at " +
                                                 std::to_string( Node.Bits ) + " bits" ) );
                    }
                    return Float;
                }

                if ( Node.Bits == 0 )
                {
                    static_cast<void>( Fail( "llvm: primitive '" + std::string( Spelling ) + "' declares a width of 0 bits" ) );
                    return nullptr;
                }
                return llvm::Type::getIntNTy( Context, Node.Bits );
            },
            [this] ( const Sema::Pointer & ) -> llvm::Type * { return llvm::PointerType::get( Context, 0 ); },
            [this, Id] ( const Sema::Aggregate &Node ) -> llvm::Type *
            {
                std::vector<llvm::Type *> Fields;
                Fields.reserve( Node.Fields.Size() );
                for ( const Sema::FieldLayout &Field : Node.Fields )
                {
                    llvm::Type *Inner = TypeOfLayout( Field.Type );
                    if ( Inner == nullptr )
                    {
                        static_cast<void>( Fail( "llvm: aggregate field '" + std::string( Build->Types->Text( Field.Name ) ) +
                                                 "' has no resolved layout" ) );
                        return nullptr;
                    }
                    Fields.push_back( Inner );
                }

                // Never packed and never reordered: declaration order at
                // natural alignment is what makes an @[External] C struct
                // work (.agents/backend/abi.md).
                llvm::StructType *Shape = llvm::StructType::get( Context, Fields, false );
                VerifyAggregateAbi( Id, Shape );
                return Shape;
            } },
        Store.Get( Id ) );

    if ( Result != nullptr )
    {
        TypeCache.emplace( Id.Value, Result );
    }
    return Result;
}

void Volt::Backend::Llvm::LlvmBackend::State::FlattenValueType ( const Sema::UnitTypes &Values,
                                                                 Sema::SemaTypeId Id,
                                                                 std::vector<std::uint32_t> &Out ) const
{
    if ( not Values.Has( Id ) )
    {
        return;
    }

    const Sema::SemaType &Value = Values.Get( Id );
    Out.push_back( Value.Base.Value );
    Out.push_back( static_cast<std::uint32_t>( Value.Args.Size() ) );
    for ( const Sema::SemaTypeId Arg : Value.Args )
    {
        FlattenValueType( Values, Arg, Out );
    }
}

Volt::Sema::LayoutId Volt::Backend::Llvm::LlvmBackend::State::LayoutOfValue ( const Sema::UnitTypes &Values, Sema::SemaTypeId Id )
{
    if ( not Values.Has( Id ) or Build == nullptr or Build->Types == nullptr )
    {
        return Sema::LayoutId{};
    }

    // The head's own arguments are what an instantiation is keyed on; the two
    // header words in front of them belong to the head itself.
    std::vector<std::uint32_t> Flat;
    FlattenValueType( Values, Id, Flat );
    if ( Flat.size() < 2 )
    {
        return Sema::LayoutId{};
    }

    return Instances.Of( *Build->Types, Sema::NominalId{ Flat[0] }, std::span<const std::uint32_t>{ Flat }.subspan( 2 ) );
}

Volt::Sema::LayoutId Volt::Backend::Llvm::LlvmBackend::State::LayoutOfExpr ( Frontend::ExprId Id )
{
    if ( Frame.Unit == nullptr or Frame.Values == nullptr )
    {
        return Sema::LayoutId{};
    }
    return LayoutOfValue( *Frame.Values, Frame.Values->ExprType( Id ) );
}

llvm::Type *Volt::Backend::Llvm::LlvmBackend::State::TypeOfExpr ( Frontend::ExprId Id )
{
    return TypeOfLayout( LayoutOfExpr( Id ) );
}

bool Volt::Backend::Llvm::LlvmBackend::State::IsAggregate ( Sema::LayoutId Id ) const
{
    if ( not Id.IsValid() or Build == nullptr or Build->Types == nullptr )
    {
        return false;
    }
    return Sema::KindOf( Build->Types->Get( Id ) ) == Sema::LayoutKind::Aggregate;
}

void Volt::Backend::Llvm::LlvmBackend::State::VerifyAggregateAbi ( [[maybe_unused]] Sema::LayoutId Id,
                                                                   [[maybe_unused]] llvm::StructType *Shape )
{
#ifndef NDEBUG
    // LayoutEngine is the single ABI authority and every target reads its
    // numbers; LLVM computes its own from the DataLayout. They must agree, and
    // a divergence is silent by nature — it corrupts a C struct field by field
    // with no diagnostic anywhere. So it is checked on every aggregate the
    // emitter creates, once, at the moment the shape is minted.
    if ( Shape == nullptr or not Layouts.has_value() or Mod == nullptr )
    {
        return;
    }

    const llvm::StructLayout *Native = Mod->getDataLayout().getStructLayout( Shape );
    const SizeAlign Ours             = Layouts->Of( Id );

    if ( Native->getSizeInBytes() != Ours.Size or Native->getAlignment().value() != Ours.Alignment )
    {
        static_cast<void>( Fail( "llvm: ABI divergence on layout " + std::to_string( Id.Value ) + ": LayoutEngine says size " +
                                 std::to_string( Ours.Size ) + " align " + std::to_string( Ours.Alignment ) +
                                 ", LLVM says size " + std::to_string( Native->getSizeInBytes() ) + " align " +
                                 std::to_string( Native->getAlignment().value() ) ) );
        return;
    }

    for ( unsigned Index = 0; Index < Shape->getNumElements(); ++Index )
    {
        const std::size_t Theirs = Native->getElementOffset( Index );
        const std::size_t Mine   = Layouts->FieldOffset( Id, Index );
        if ( Theirs != Mine )
        {
            static_cast<void>( Fail( "llvm: ABI divergence on layout " + std::to_string( Id.Value ) + " field " +
                                     std::to_string( Index ) + ": LayoutEngine says offset " + std::to_string( Mine ) +
                                     ", LLVM says " + std::to_string( Theirs ) ) );
            return;
        }
    }
#endif
}
