// ExprLiteralEmitter.cpp — the terminals, and the lexeme decoding they need.
//
// The parser keeps a literal's raw lexeme and leaves decoding to whoever needs
// a value (Expr.hpp): `volt parse` and the golden fixtures show source text, so
// decoding belongs where bytes are actually materialised, which is here.
//
// The *width* never comes from the lexeme. It comes from the type that claimed
// the node kind through `@[Literal]`, resolved by Sema onto this very
// expression — a `_u64`-style suffix is parsed and then ignored, which is a
// recorded middle-end gap (rules/core-ast.md: `0_u64` types as the IntLiteral
// claimant), and honouring it here would make the backend disagree with the
// type Sema assigned.

#include "Lower/Expr/ExprEmitter.hpp"

#include "Core/DiagnosticSink.hpp"
#include "Core/EmitterServices.hpp"
#include "Core/ModuleContext.hpp"
#include "Lower/BodyEmitter.hpp"
#include "Lower/FunctionFrame.hpp"
#include "Types/TypeMapper.hpp"

#include "Volt/BackendCore/SymbolRegistry.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

#include <charconv>
#include <cstdint>
#include <string>

namespace
{

// Underscores are separators anywhere in the digits; a trailing type suffix is
// trimmed. A suffix opens with the same character a separator uses, so the two
// are told apart by what follows: `1_000` continues in digits, `0_u64` does not.
[[nodiscard]] bool DecodeInteger ( std::string_view Text, std::uint64_t &Out )
{
    int Radix = 10;
    if ( Text.size() > 2 and Text[0] == '0' )
    {
        switch ( Text[1] )
        {
        case 'x':
        case 'X':
            Radix = 16;
            Text  = Text.substr( 2 );
            break;
        case 'b':
        case 'B':
            Radix = 2;
            Text  = Text.substr( 2 );
            break;
        case 'o':
        case 'O':
            Radix = 8;
            Text  = Text.substr( 2 );
            break;
        default:
            break;
        }
    }

    std::string Digits;
    Digits.reserve( Text.size() );
    for ( const char Ch : Text )
    {
        if ( Ch == '_' )
        {
            continue;
        }
        Digits.push_back( Ch );
    }

    // Trim a trailing type suffix: everything from the first character that is
    // not a digit of this radix.
    std::size_t End = 0;
    while ( End < Digits.size() )
    {
        const char Ch = Digits[End];
        int Value     = 99;
        if ( Ch >= '0' and Ch <= '9' )
        {
            Value = Ch - '0';
        }
        else if ( Ch >= 'a' and Ch <= 'f' )
        {
            Value = Ch - 'a' + 10;
        }
        else if ( Ch >= 'A' and Ch <= 'F' )
        {
            Value = Ch - 'A' + 10;
        }
        if ( Value >= Radix )
        {
            break;
        }
        ++End;
    }
    if ( End == 0 )
    {
        return false;
    }

    const char *Begin       = Digits.data();
    const auto [Ptr, Error] = std::from_chars( Begin, Begin + End, Out, Radix );
    return Error == std::errc{} and Ptr == Begin + End;
}

[[nodiscard]] bool DecodeFloat ( std::string_view Text, double &Out )
{
    std::string Digits;
    Digits.reserve( Text.size() );
    for ( const char Ch : Text )
    {
        if ( Ch != '_' )
        {
            Digits.push_back( Ch );
        }
    }

    const char *Begin       = Digits.data();
    const auto [Ptr, Error] = std::from_chars( Begin, Begin + Digits.size(), Out );
    return Error == std::errc{} and Ptr != Begin;
}

// The escape alphabet, in one place. It is the *lexer's*, not a Volt type's
// (rules/zero-hardcode.md is about type names; `\n` is lexical syntax), and both
// literal forms that can contain one read it here — a char literal is a
// one-character string as far as escaping goes, so two tables would be two
// chances to disagree.
[[nodiscard]] constexpr int HexVal ( char C ) noexcept
{
    if ( C >= '0' and C <= '9' )
    {
        return C - '0';
    }
    if ( C >= 'a' and C <= 'f' )
    {
        return C - 'a' + 10;
    }
    if ( C >= 'A' and C <= 'F' )
    {
        return C - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] constexpr bool DecodeEscape ( char Spelled, char &Out ) noexcept
{
    switch ( Spelled )
    {
    case 'a':
        Out = '\a';
        return true;
    case 'b':
        Out = '\b';
        return true;
    case 'n':
        Out = '\n';
        return true;
    case 't':
        Out = '\t';
        return true;
    case 'v':
        Out = '\v';
        return true;
    case 'f':
        Out = '\f';
        return true;
    case 'r':
        Out = '\r';
        return true;
    case '0':
        Out = '\0';
        return true;
    case 'e':
        Out = '\x1b';
        return true;
    case '\\':
    case '\'':
    case '"':
        Out = Spelled;
        return true;
    default:
        return false;
    }
}

// A char literal's raw lexeme, escapes resolved.
[[nodiscard]] bool DecodeChar ( std::string_view Text, std::uint64_t &Out )
{
    if ( Text.size() >= 2 and ( Text.front() == '\'' or Text.front() == '"' ) )
    {
        Text = Text.substr( 1, Text.size() - 2 );
    }
    if ( Text.empty() )
    {
        return false;
    }

    if ( Text.front() != '\\' )
    {
        Out = static_cast<std::uint8_t>( Text.front() );
        return true;
    }
    if ( Text.size() < 2 )
    {
        return false;
    }

    if ( ( Text[1] == 'x' or Text[1] == 'X' ) and Text.size() > 2 )
    {
        const int H1 = HexVal( Text[2] );
        if ( H1 < 0 )
        {
            return false;
        }
        int Val = H1;
        if ( Text.size() > 3 )
        {
            const int H2 = HexVal( Text[3] );
            if ( H2 < 0 )
            {
                return false;
            }
            Val = ( Val << 4 ) | H2;
        }
        Out = static_cast<std::uint8_t>( Val );
        return true;
    }

    char Decoded = '\0';
    if ( not DecodeEscape( Text[1], Decoded ) )
    {
        return false;
    }
    Out = static_cast<std::uint8_t>( Decoded );
    return true;
}

// A string literal's raw lexeme, escapes resolved. An unrecognised escape keeps
// both characters rather than being refused: the lexer already accepted the
// literal, and a backend does not diagnose Volt source
// (.agents/backend/core-interfaces.md).
[[nodiscard]] std::string DecodeText ( std::string_view Text )
{
    std::string Out;
    Out.reserve( Text.size() );
    for ( std::size_t Index = 0; Index < Text.size(); ++Index )
    {
        if ( Text[Index] == '\\' and Index + 1 < Text.size() )
        {
            const char Next = Text[Index + 1];
            if ( ( Next == 'x' or Next == 'X' ) and Index + 2 < Text.size() )
            {
                const int H1 = HexVal( Text[Index + 2] );
                if ( H1 >= 0 )
                {
                    int Val              = H1;
                    std::size_t Advanced = 2;
                    if ( Index + 3 < Text.size() )
                    {
                        const int H2 = HexVal( Text[Index + 3] );
                        if ( H2 >= 0 )
                        {
                            Val      = ( Val << 4 ) | H2;
                            Advanced = 3;
                        }
                    }
                    Out.push_back( static_cast<char>( Val ) );
                    Index += Advanced;
                    continue;
                }
            }

            char Decoded = '\0';
            if ( DecodeEscape( Next, Decoded ) )
            {
                Out.push_back( Decoded );
                ++Index;
                continue;
            }
        }
        Out.push_back( Text[Index] );
    }
    return Out;
}

} // namespace

llvm::Value *Volt::Backend::Llvm::EmitIntLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::IntLiteral &Node )
{
    llvm::Type *Shape = Emitter.TypeOfExpr( Id );
    if ( Shape == nullptr or not( Shape->isIntegerTy() or Shape->isFloatingPointTy() ) )
    {
        static_cast<void>( Emitter.Fail( "llvm: integer literal '" + std::string( Emitter.Frame().Unit->Ast->Text( Node.Raw ) ) +
                                         "' has no integer layout" ) );
        return nullptr;
    }

    std::uint64_t Value = 0;
    if ( not DecodeInteger( Emitter.Frame().Unit->Ast->Text( Node.Raw ), Value ) )
    {
        static_cast<void>( Emitter.Fail( "llvm: cannot decode integer literal '" +
                                         std::string( Emitter.Frame().Unit->Ast->Text( Node.Raw ) ) + "'" ) );
        return nullptr;
    }

    // `self == 0` inside a generic `Arithmetic` default (`zero?`, `abs`, ...) is
    // unconstrained until instantiation, and adopts whatever `self` turns out to
    // be — a bare digit sequence is as much an unconstrained literal as a
    // `FloatLiteral` when the receiver is `Float32`/`Float64`
    // (TypeCheckerConstraint's `Expected` propagation types it there, never this
    // emitter), so the same node must be able to materialise either constant
    // kind.
    if ( Shape->isFloatingPointTy() )
    {
        return llvm::ConstantFP::get( Shape, static_cast<double>( Value ) );
    }
    return llvm::ConstantInt::get( Shape, Value );
}

llvm::Value *
Volt::Backend::Llvm::EmitFloatLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::FloatLiteral &Node )
{
    llvm::Type *Shape = Emitter.TypeOfExpr( Id );
    if ( Shape == nullptr or not Shape->isFloatingPointTy() )
    {
        static_cast<void>( Emitter.Fail( "llvm: float literal '" + std::string( Emitter.Frame().Unit->Ast->Text( Node.Raw ) ) +
                                         "' has no floating-point layout" ) );
        return nullptr;
    }

    double Value = 0.0;
    if ( not DecodeFloat( Emitter.Frame().Unit->Ast->Text( Node.Raw ), Value ) )
    {
        static_cast<void>( Emitter.Fail( "llvm: cannot decode float literal '" +
                                         std::string( Emitter.Frame().Unit->Ast->Text( Node.Raw ) ) + "'" ) );
        return nullptr;
    }
    return llvm::ConstantFP::get( Shape, Value );
}

llvm::Value *Volt::Backend::Llvm::EmitCharLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::CharLiteral &Node )
{
    llvm::Type *Shape = Emitter.TypeOfExpr( Id );
    if ( Shape == nullptr or not Shape->isIntegerTy() )
    {
        static_cast<void>( Emitter.Fail( "llvm: the type claiming CharLiteral has no integer layout" ) );
        return nullptr;
    }

    std::uint64_t Value = 0;
    if ( not DecodeChar( Emitter.Frame().Unit->Ast->Text( Node.Raw ), Value ) )
    {
        static_cast<void>( Emitter.Fail( "llvm: cannot decode character literal '" +
                                         std::string( Emitter.Frame().Unit->Ast->Text( Node.Raw ) ) + "'" ) );
        return nullptr;
    }
    return llvm::ConstantInt::get( Shape, Value );
}

llvm::Value *
Volt::Backend::Llvm::EmitStringLiteral ( BodyEmitter &Emitter, Frontend::ExprId /* Id */, const Frontend::StringLiteral &Node )
{
    // StringLiteral evaluated in codegen is a raw byte buffer constant
    // (Pointer<UInt8>). The aggregate String struct construction
    // String.new( bytes, size ) was already lowered upstream by LowerStringLits
    // in TypeChecker.
    const std::string Text = DecodeText( Emitter.Frame().Unit->Ast->Text( Node.Value ) );
    return Emitter.Ctx().Builder().CreateGlobalString( Text, ".str", 0, Emitter.Ctx().ModPtr() );
}

llvm::Value *Volt::Backend::Llvm::EmitBoolLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::BoolLiteral &Node )
{
    llvm::Type *Shape = Emitter.TypeOfExpr( Id );
    if ( Shape == nullptr or not Shape->isIntegerTy() )
    {
        static_cast<void>( Emitter.Fail( "llvm: the type claiming BoolLiteral has no integer layout" ) );
        return nullptr;
    }
    return llvm::ConstantInt::get( Shape, Node.Value ? 1 : 0 );
}

llvm::Value *Volt::Backend::Llvm::EmitNilLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id )
{
    // Null of whatever shape the type claiming NilLiteral resolves to — a `ptr`
    // today, and nothing here depends on that.
    llvm::Type *Shape = Emitter.TypeOfExpr( Id );
    if ( Shape == nullptr )
    {
        static_cast<void>( Emitter.Fail( "llvm: the type claiming NilLiteral has no resolved layout" ) );
        return nullptr;
    }
    return llvm::Constant::getNullValue( Shape );
}

llvm::Value *
Volt::Backend::Llvm::EmitSymbolLiteral ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::SymbolLiteral &Node )
{
    // The identity is minted from the *name*, never from `Node.Name` itself:
    // that handle is interned per file (rules/ast-value.md), so emitting it gave
    // `:pending` one integer in the unit that wrote it and a different one in
    // the unit that compared against it. SymbolRegistry owns the one answer, and
    // the same one names it back in `_V_symbol_name`.
    llvm::Type *Shape = Emitter.TypeOfExpr( Id );
    if ( Shape == nullptr or not Shape->isIntegerTy() )
    {
        static_cast<void>( Emitter.Fail( "llvm: the type claiming SymbolLiteral has no integer layout" ) );
        return nullptr;
    }
    return llvm::ConstantInt::get( Shape, SymbolValueOf( SymbolNameOf( Emitter.Frame().Unit->Ast->Text( Node.Name ) ) ) );
}

llvm::Value *Volt::Backend::Llvm::EmitSizeOf ( BodyEmitter &Emitter, Frontend::ExprId Id )
{
    FunctionFrame &Frame = Emitter.Frame();

    // `sizeof T` names a type in a *TypeId*, which is a spelling — resolving it
    // here would be semantic analysis in codegen. TypeChecker publishes the
    // measured type on this node's own site instead, so all that is left is to
    // measure its layout, which is LayoutEngine's answer and nobody else's.
    auto Site                                            = Frame.Values->SiteType( MiddleEnd::Resolver::BindingSite{ Id } );
    const MiddleEnd::TypeSystem::UnitTypes *SourceValues = Frame.Values;
    if ( not Site.IsValid() and Frame.Unit != nullptr and Frame.Unit->Values != nullptr )
    {
        Site         = Frame.Unit->Values->SiteType( MiddleEnd::Resolver::BindingSite{ Id } );
        SourceValues = Frame.Unit->Values;
    }
    const MiddleEnd::TypeSystem::LayoutId Shape = Emitter.Types().LayoutOfValue( *SourceValues, Site );
    if ( not Shape.IsValid() )
    {
        static_cast<void>( Emitter.Fail( "llvm: `sizeof` at expression " + std::to_string( Id.Value ) +
                                         " — its operand names a type with no resolved layout" ) );
        return nullptr;
    }

    // The width is the one the *use site* gave the expression, exactly as for an
    // integer literal: `count * sizeof T` against a UInt64 count is a UInt64
    // constant.
    llvm::Type *Width = Emitter.TypeOfExpr( Id );
    if ( Width == nullptr or not Width->isIntegerTy() )
    {
        static_cast<void>(
            Emitter.Fail( "llvm: `sizeof` at expression " + std::to_string( Id.Value ) + " has no integer layout" ) );
        return nullptr;
    }
    if ( not Emitter.Services().Layouts->has_value() )
    {
        static_cast<void>(
            Emitter.Fail( "llvm: `sizeof` needs the layout engine, which the target was never initialised with" ) );
        return nullptr;
    }
    return llvm::ConstantInt::get( Width, ( *Emitter.Services().Layouts )->Of( Shape ).Size );
}

llvm::Value *Volt::Backend::Llvm::EmitTypeTrait ( BodyEmitter &Emitter, Frontend::ExprId Id, const Frontend::TypeTrait &Node )
{
    FunctionFrame &Frame = Emitter.Frame();

    // Exactly `sizeof`'s shape one line up: the operand is a *spelling*,
    // TypeChecker published the type it resolves to on this node's own site,
    // and all that is left is to read a settled fact off the type record and
    // materialise it. Nothing is decided here — deciding what
    // `trivially_destructible?` means is the seam's job
    // (`SynthesizeFinalizeStubs`), read back through one accessor.
    const MiddleEnd::TypeSystem::SemaTypeId Operand = Frame.Values->SiteType( MiddleEnd::Resolver::BindingSite{ Id } );

    llvm::Type *Shape = Emitter.TypeOfExpr( Id );
    if ( Shape == nullptr or not Shape->isIntegerTy() )
    {
        static_cast<void>(
            Emitter.Fail( "llvm: type predicate at expression " + std::to_string( Id.Value ) + " has no integer layout" ) );
        return nullptr;
    }

    const MiddleEnd::TypeSystem::TypeStore &Store = *Emitter.Services().Build->Types;

    bool Answer = false;
    switch ( Node.Trait )
    {
    case Frontend::TokenKind::KwTriviallyDestructible:
        Answer = Operand.IsValid() and Frame.Values->Has( Operand ) and
                 Store.IsTriviallyDestructible( Frame.Values->Get( Operand ).Base );
        break;
    default:
        static_cast<void>( Emitter.Fail( "llvm: type predicate at expression " + std::to_string( Id.Value ) +
                                         " names a trait this target does not answer" ) );
        return nullptr;
    }

    return llvm::ConstantInt::get( Shape, static_cast<std::uint64_t>( Answer ) );
}
