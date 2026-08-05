// TypeMapper.cpp — LayoutId -> llvm::Type*.
//
// See TypeMapper.hpp. The mapping reads a layout and nothing else; the ABI
// cross-check it triggers on every aggregate lives in AbiVerifier.cpp, and the
// SemaTypeId -> LayoutId half in LayoutOfValue.cpp.

#include "Types/TypeMapper.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/AbiVerifier.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <cstdint>
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

llvm::Type *Volt::Backend::Llvm::TypeMapper::TypeOfLayout ( Sema::LayoutId Id )
{
    if ( not Id.IsValid() or Services->Build == nullptr or Services->Build->Types == nullptr )
    {
        return nullptr;
    }

    if ( const auto It = Cache.find( Id.Value ); It != Cache.end() )
    {
        return It->second;
    }

    const Sema::TypeStore &Store = *Services->Build->Types;
    llvm::LLVMContext &Context   = Services->Ctx->Context();

    llvm::Type *Result = std::visit(
        Meta::Overloaded{
            [] ( const std::monostate & ) -> llvm::Type * { return nullptr; },
            [this, &Store, &Context] ( const Sema::Primitive &Node ) -> llvm::Type *
            {
                const std::string_view Spelling = Node.Spelling.IsValid() ? Store.Text( Node.Spelling ) : std::string_view{};
                if ( Spelling.empty() )
                {
                    static_cast<void>( Services->Diag->Fail( "llvm: a primitive layout carries no spelling" ) );
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
                        static_cast<void>( Services->Diag->Fail( "llvm: no machine float type for '" + std::string( Spelling ) +
                                                                 "' at " + std::to_string( Node.Bits ) + " bits" ) );
                    }
                    return Float;
                }

                if ( Node.Bits == 0 )
                {
                    static_cast<void>(
                        Services->Diag->Fail( "llvm: primitive '" + std::string( Spelling ) + "' declares a width of 0 bits" ) );
                    return nullptr;
                }
                return llvm::Type::getIntNTy( Context, Node.Bits );
            },
            [&Context] ( const Sema::Pointer & ) -> llvm::Type * { return llvm::PointerType::get( Context, 0 ); },
            [this, Id, &Context] ( const Sema::Aggregate &Node ) -> llvm::Type *
            {
                std::vector<llvm::Type *> Fields;
                Fields.reserve( Node.Fields.Size() );
                for ( const Sema::FieldLayout &Field : Node.Fields )
                {
                    llvm::Type *Inner = TypeOfLayout( Field.Type );
                    if ( Inner == nullptr )
                    {
                        static_cast<void>( Services->Diag->Fail( "llvm: aggregate field '" +
                                                                 std::string( Services->Build->Types->Text( Field.Name ) ) +
                                                                 "' has no resolved layout" ) );
                        return nullptr;
                    }
                    Fields.push_back( Inner );
                }

                // Never packed and never reordered: declaration order at natural
                // alignment is what makes an @[External] C struct work
                // (.agents/backend/abi.md).
                llvm::StructType *Shape = llvm::StructType::get( Context, Fields, false );
                Services->Abi->VerifyAggregateAbi( Id, Shape );
                return Shape;
            } },
        Store.Get( Id ) );

    if ( Result != nullptr )
    {
        Cache.emplace( Id.Value, Result );
    }
    return Result;
}

llvm::Type *Volt::Backend::Llvm::TypeMapper::ParamTypeOfLayout ( Sema::LayoutId Id )
{
    llvm::Type *Shape = TypeOfLayout( Id );
    if ( Shape == nullptr )
    {
        return nullptr;
    }
    return Shape->isStructTy() ? llvm::PointerType::get( Services->Ctx->Context(), 0 ) : Shape;
}

Volt::Sema::LayoutId Volt::Backend::Llvm::TypeMapper::LayoutOfExpr ( const FunctionFrame &Frame, Frontend::ExprId Id )
{
    if ( Frame.Unit == nullptr or Frame.Values == nullptr )
    {
        return Sema::LayoutId{};
    }
    return LayoutOfValue( *Frame.Values, Frame.Values->ExprType( Id ) );
}

llvm::Type *Volt::Backend::Llvm::TypeMapper::TypeOfExpr ( const FunctionFrame &Frame, Frontend::ExprId Id )
{
    return TypeOfLayout( LayoutOfExpr( Frame, Id ) );
}

bool Volt::Backend::Llvm::TypeMapper::IsAggregate ( Sema::LayoutId Id ) const
{
    if ( not Id.IsValid() or Services->Build == nullptr or Services->Build->Types == nullptr )
    {
        return false;
    }
    return Sema::KindOf( Services->Build->Types->Get( Id ) ) == Sema::LayoutKind::Aggregate;
}

std::string_view Volt::Backend::Llvm::TypeMapper::SpellingOf ( Sema::LayoutId Id ) const
{
    if ( not Id.IsValid() or Services->Build == nullptr or Services->Build->Types == nullptr )
    {
        return {};
    }
    const Sema::LayoutNode &Node = Services->Build->Types->Get( Id );
    if ( const auto *Scalar = std::get_if<Sema::Primitive>( &Node ); Scalar != nullptr and Scalar->Spelling.IsValid() )
    {
        return Services->Build->Types->Text( Scalar->Spelling );
    }
    // A Pointer layout is an address just as `@[Primitive("ptr")]` is, and the
    // two must select the same instructions.
    return std::holds_alternative<Sema::Pointer>( Node ) ? std::string_view{ "ptr" } : std::string_view{};
}
