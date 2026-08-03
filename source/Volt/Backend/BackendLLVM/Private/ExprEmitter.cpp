// ExprEmitter.cpp — the value-producing core nodes.
//
// One `std::visit` site for the whole category (core-interfaces.md): per-node
// dispatch is never virtual and never a `switch` over kinds, and a node the
// contract says cannot reach a backend falls into the `auto` arm and is
// reported by name rather than guessed at.
//
// Two conventions hold throughout, and everything else follows from them:
//
//   - A scalar evaluates to a register; an **aggregate evaluates to a `ptr`**
//     at its storage (abi.md: aggregates by pointer). So `EmitExpr` on a
//     struct-shaped expression hands back an address, and `EmitStore` moves it
//     with a memcpy sized by LayoutEngine.
//   - Nothing here decides a type. Every shape comes from `Values->ExprType`
//     through InstanceLayouts, every callee from `Callees->Get`, every binding
//     from `Scopes->BindingOf`. When one of those is missing the emitter says
//     so and stops — a middle-end whose own invariants (rules/core-ast.md) say
//     it cannot happen is worth a loud report, never a repair.

#include "LlvmState.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

#include <charconv>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace
{

using namespace Volt;

// --- The primitive instruction manifest ------------------------------------

// The family a spelling belongs to. Derived from one character, which is the
// whole voca
// using nambulary rules/zero-hardcode.md grants the C++ side: `"i32"` selects
// signed instructions because it starts with `i`, not because anything here
// knows it was written `Int32`.
enum class EOpFamily : std::uint8_t
{

    None,
    SInt,
    UInt,
    Float,
};

// The unary forms, which are not BinaryOps rows: each needs the operand's own
// width or type to build its constant.
enum class EUnaryOp : std::uint8_t
{

    Neg,        // 0 - x
    FNeg,       // fneg x
    BitNot,     // x xor -1
    LogicalNot, // x xor true — the i1 case
};

struct BinOpRow
{

    EOpFamily Family = EOpFamily::None;
    Frontend::TokenKind Op{};
    llvm::Instruction::BinaryOps Opcode{};
};

struct CmpRow
{

    EOpFamily Family = EOpFamily::None;
    Frontend::TokenKind Op{};
    llvm::CmpInst::Predicate Predicate{};
};

struct UnOpRow
{

    EOpFamily Family = EOpFamily::None;
    Frontend::TokenKind Op{};
    EUnaryOp Kind{};
};

constexpr BinOpRow BinOps[] = {
#define VOLT_LLVM_BINOP( Family, Token, Opcode )                                                                                 \
    BinOpRow{ EOpFamily::Family, Frontend::TokenKind::Token, llvm::Instruction::BinaryOps::Opcode },
#include "Instructions.inl"
};

constexpr CmpRow Cmps[] = {
#define VOLT_LLVM_CMP( Family, Token, Predicate )                                                                                \
    CmpRow{ EOpFamily::Family, Frontend::TokenKind::Token, llvm::CmpInst::Predicate },
#include "Instructions.inl"
};

constexpr UnOpRow UnOps[] = {
#define VOLT_LLVM_UNOP( Family, Token, Kind ) UnOpRow{ EOpFamily::Family, Frontend::TokenKind::Token, EUnaryOp::Kind },
#include "Instructions.inl"
};

template <typename Row> [[nodiscard]] const Row *FindRow ( std::span<const Row> Rows, EOpFamily Family, Frontend::TokenKind Op )
{
    for ( const Row &Entry : Rows )
    {
        if ( Entry.Family == Family and Entry.Op == Op )
        {
            return &Entry;
        }
    }
    return nullptr;
}

// The family of an opaque spelling. `"ptr"` joins the unsigned integers: an
// address has no sign bit, so `p < q` is `icmp ult` — and pointer `+`/`-` never
// reach a table row at all, being address arithmetic.
[[nodiscard]] EOpFamily FamilyOf ( std::string_view Spelling )
{
    if ( Spelling.empty() )
    {
        return EOpFamily::None;
    }
    if ( Spelling == "ptr" )
    {
        return EOpFamily::UInt;
    }

    switch ( Spelling.front() )
    {
    case 'i':
        return EOpFamily::SInt;
    case 'u':
        return EOpFamily::UInt;
    case 'f':
        return EOpFamily::Float;
    default:
        return EOpFamily::None;
    }
}

// --- Literal lexemes -------------------------------------------------------

// The parser keeps a literal's raw lexeme and leaves decoding to whoever needs
// a value (Expr.hpp). Underscores are separators anywhere in the digits, and a
// `_u64`-style suffix is *parsed and then ignored* — that is a recorded
// middle-end gap (rules/core-ast.md: `0_u64` types as the IntLiteral claimant),
// and honouring it here would make the backend disagree with the type Sema
// assigned. The width comes from the layout, always.
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
            // A suffix opens with the same character a separator uses, so the
            // two are told apart by what follows: `1_000` continues in digits,
            // `0_u64` does not.
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
// (rules/zero-hardcode.md is about type names; `\n` is lexical syntax), and
// both literal forms that can contain one read it here — a char literal is a
// one-character string as far as escaping goes, so two tables would be two
// chances to disagree.
//
// The lexer deliberately interns the raw spelling: `volt parse` and the golden
// fixtures show source text. Decoding therefore belongs where bytes are
// actually materialised, which is here.
[[nodiscard]] bool DecodeEscape ( char Spelled, char &Out )
{
    switch ( Spelled )
    {
    case 'n':
        Out = '\n';
        return true;
    case 't':
        Out = '\t';
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

    char Decoded = '\0';
    if ( not DecodeEscape( Text[1], Decoded ) )
    {
        return false;
    }
    Out = static_cast<std::uint8_t>( Decoded );
    return true;
}

// A string literal's raw lexeme, escapes resolved. An unrecognised escape
// keeps both characters rather than being refused: the lexer already accepted
// the literal, and a backend does not diagnose Volt source
// (.agents/backend/core-interfaces.md).
[[nodiscard]] std::string DecodeText ( std::string_view Text )
{
    std::string Out;
    Out.reserve( Text.size() );
    for ( std::size_t Index = 0; Index < Text.size(); ++Index )
    {
        char Decoded = '\0';
        if ( Text[Index] == '\\' and Index + 1 < Text.size() and DecodeEscape( Text[Index + 1], Decoded ) )
        {
            Out.push_back( Decoded );
            ++Index;
            continue;
        }
        Out.push_back( Text[Index] );
    }
    return Out;
}

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

// ---------------------------------------------------------------------------
// Places
// ---------------------------------------------------------------------------

std::string Volt::Backend::Llvm::LlvmBackend::State::MissingSelf ( std::string_view What ) const
{
    if ( Frame.bClosure )
    {
        return "llvm: " + std::string( What ) +
               " inside a closure body — SynthesizeClosureFrame captures bindings, and a receiver is not one, so nothing "
               "carries `self` into the environment";
    }
    return "llvm: " + std::string( What ) + " outside a method with a receiver";
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitAddress ( Frontend::ExprId Id )
{
    if ( Failed() or Frame.Unit == nullptr or not Id.IsValid() )
    {
        return nullptr;
    }

    const Frontend::AstContext &Ast = *Frame.Unit->Ast;

    return std::visit(
        Meta::Overloaded{
            [this, Id] ( const Frontend::Identifier &Node ) -> llvm::Value *
            {
                // The binding is ScopeResolver's answer, never a re-resolution:
                // a name the resolver did not bind is not a name this emitter
                // may go looking for.
                const Sema::Binding *Bound = Frame.Unit->Scopes->BindingOf( Id );
                if ( Bound == nullptr )
                {
                    static_cast<void>( Fail( "llvm: identifier '" + std::string( Frame.Unit->Ast->Text( Node.Name ) ) +
                                             "' reached codegen with no scope binding" ) );
                    return nullptr;
                }

                if ( const auto It = Frame.Slots.find( Bound->Site ); It != Frame.Slots.end() )
                {
                    return It->second;
                }

                if ( Bound->Owner.IsValid() and
                     ( Frame.Unit->Scopes->Get( Bound->Owner ).Kind == Sema::EScopeKind::Unit or
                       Frame.Unit->Scopes->Get( Bound->Owner ).Kind == static_cast<Sema::EScopeKind>( 0 ) ) )
                {
                    const Sema::LayoutId Shape = LayoutOfValue( *Frame.Values, Frame.Values->SiteType( Bound->Site ) );
                    llvm::Type *Slot           = TypeOfLayout( Shape );
                    if ( Slot == nullptr )
                    {
                        static_cast<void>( Fail( "llvm: unit global '" + std::string( Frame.Unit->Ast->Text( Bound->Name ) ) +
                                                 "' has no resolved layout" ) );
                        return nullptr;
                    }
                    return SlotFor( Bound->Site, Slot, Frame.Unit->Ast->Text( Bound->Name ) );
                }

                // An implicit local (`buf = expr`, no `: Type`) has no
                // declaring *statement* to open its storage the way a
                // LocalDecl does, so the occurrence ScopeResolver recorded as
                // its site — the Assign target, and no other — opens it here.
                // Everything else about it is a local like any other: the
                // shape is the one TypeChecker recorded for the site.
                if ( const auto *Site = std::get_if<Frontend::ExprId>( &Bound->Site ); Site != nullptr )
                {
                    const Sema::LayoutId Shape = LayoutOfValue( *Frame.Values, Frame.Values->SiteType( Bound->Site ) );
                    llvm::Type *Slot           = TypeOfLayout( Shape );
                    if ( Slot == nullptr )
                    {
                        static_cast<void>( Fail( "llvm: local '" + std::string( Frame.Unit->Ast->Text( Bound->Name ) ) +
                                                 "' has no resolved layout" ) );
                        return nullptr;
                    }
                    return SlotFor( Bound->Site, Slot, Frame.Unit->Ast->Text( Bound->Name ) );
                }

                // A local declared in a branch the walk has not reached is
                // impossible, and a captured one was bound from the
                // environment on entry — so what is left is a name
                // ScopeResolver bound outside this body without recording
                // it as a capture.
                static_cast<void>( Fail( "llvm: '" + std::string( Frame.Unit->Ast->Text( Bound->Name ) ) +
                                         "' has no slot in this function, and no capture of it was recorded" ) );
                return nullptr;
            },
            [this, Id] ( const Frontend::InstanceVar &Node ) -> llvm::Value *
            {
                // The written spelling keeps its `@` (the parser interns the
                // token whole); a field is declared without one. Sema strips
                // it in exactly one place too — LookupOn's CleanName — and the
                // two must agree, or a member resolves and its storage does
                // not.
                const std::string_view Field = FieldNameOf( Frame.Unit->Ast->Text( Node.Name ) );
                if ( Frame.Self == nullptr )
                {
                    static_cast<void>( Fail( MissingSelf( "'@" + std::string( Field ) + "'" ) ) );
                    return nullptr;
                }
                return FieldAddress( Frame.Self, Frame.SelfLayout, Field, Id );
            },
            [this, Id] ( const Frontend::Member &Node ) -> llvm::Value *
            {
                llvm::Value *Object = EmitExpr( Node.Object );
                if ( Object == nullptr )
                {
                    return nullptr;
                }
                return FieldAddress( Object, LayoutOfExpr( Node.Object ), Frame.Unit->Ast->Text( Node.Name ), Id );
            },
            // `*p = v`: the place *is* the pointer the operand evaluates to.
            [this] ( const Frontend::Deref &Node ) -> llvm::Value * { return EmitExpr( Node.Operand ); },
            [this, Id] ( const auto & ) -> llvm::Value *
            {
                static_cast<void>( Fail( "llvm: " + std::string( Frontend::NodeName( Frame.Unit->Ast->Expr( Id ) ) ) +
                                         " is not an assignable place" ) );
                return nullptr;
            } },
        Ast.Expr( Id ) );
}

std::string_view Volt::Backend::Llvm::LlvmBackend::State::FieldNameOf ( std::string_view Written )
{
    return Written.starts_with( '@' ) ? Written.substr( 1 ) : Written;
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::FieldAddress ( llvm::Value *Object,
                                                                     Sema::LayoutId Shape,
                                                                     std::string_view Name,
                                                                     Frontend::ExprId Id )
{
    llvm::Type *Struct = TypeOfLayout( Shape );
    if ( Struct == nullptr or not Struct->isStructTy() )
    {
        static_cast<void>( Fail( "llvm: field '" + std::string( Name ) + "' on a receiver with no aggregate layout" ) );
        return nullptr;
    }

    // The index is the field's position in the *layout*, which is declaration
    // order (abi.md) — the same order TypeMapper built the LLVM struct in, and
    // the same one VerifyAggregateAbi already cross-checked against
    // LayoutEngine. So a struct GEP and LayoutEngine::FieldOffset name the same
    // byte by construction.
    const Sema::Aggregate &Fields = std::get<Sema::Aggregate>( Build->Types->Get( Shape ) );
    for ( std::size_t Index = 0; Index < Fields.Fields.Size(); ++Index )
    {
        if ( Build->Types->Text( Fields.Fields[Index].Name ) == Name )
        {
            return Builder->CreateStructGEP( Struct, Object, static_cast<unsigned>( Index ), "field." + std::string( Name ) );
        }
    }

    static_cast<void>(
        Fail( "llvm: no field '" + std::string( Name ) + "' in the layout of expression " + std::to_string( Id.Value ) ) );
    return nullptr;
}

void Volt::Backend::Llvm::LlvmBackend::State::EmitStore ( llvm::Value *Address, llvm::Value *Value, Sema::LayoutId Shape )
{
    if ( Address == nullptr or Value == nullptr or Failed() )
    {
        return;
    }

    if ( not IsAggregate( Shape ) )
    {
        // Reconciling the two widths belongs here, at the one point every store
        // passes through, rather than at each of the four call sites — only one
        // of which used to do it, so an `Assign` never coerced at all.
        //
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
            Slot = TypeOfLayout( Shape );
        }
        Value = CoerceWidth( Value, Slot );
        if ( Slot != nullptr and Value->getType() != Slot )
        {
            // CoerceWidth widens and never narrows, by design: Sema's
            // IsWideningScalar admits an i8 where a u64 is wanted and refuses
            // the reverse (rules/zero-hardcode.md). So a value still too wide
            // here means the middle-end handed out two different answers for
            // one binding — Values.ExprType( value ) against
            // Values.SiteType( slot ) — and the honest reading is a hole in
            // those tables, not a truncation for this emitter to invent.
            // Storing it anyway writes past the end of the slot and smashes
            // the frame, which is how this was found: `result = 1` typed Int32
            // against an Int8 local put a `store i32` into an `alloca i8`.
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
        static_cast<void>( Builder->CreateStore( Value, Address ) );
        return;
    }

    // An aggregate is only ever an address, so assigning one is a copy — and
    // its size is LayoutEngine's, the single ABI authority, not sizeof-anything
    // the emitter recomputes.
    const SizeAlign Measure =
        Layouts->Of( Shape ); // NOLINT(bugprone-unchecked-optional-access) — Layouts is always emplaced before any emission
    static_cast<void>( Builder->CreateMemCpy( Address, llvm::MaybeAlign( Measure.Alignment ), Value,
                                              llvm::MaybeAlign( Measure.Alignment ), Builder->getInt64( Measure.Size ) ) );
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitExpr ( Frontend::ExprId Id )
{
    // `Terminated()` here as well as in `EmitStmts`: a `raise` (or a call the
    // post-call check found pending) can end the current block mid-expression
    // — e.g. a non-tail argument — and every remaining sub-expression of the
    // one being built is genuinely unreachable. Nothing here decides that; it
    // only refuses to append instructions after it.
    if ( Failed() or Terminated() or Frame.Unit == nullptr or not Id.IsValid() )
    {
        return nullptr;
    }

    const Frontend::AstContext &Ast = *Frame.Unit->Ast;

    return std::visit(
        Meta::Overloaded{
            // --- Terminals -------------------------------------------------
            [this, Id] ( const Frontend::IntLiteral &Node ) -> llvm::Value * { return EmitIntLiteral( Id, Node ); },
            [this, Id] ( const Frontend::FloatLiteral &Node ) -> llvm::Value * { return EmitFloatLiteral( Id, Node ); },
            [this, Id] ( const Frontend::CharLiteral &Node ) -> llvm::Value * { return EmitCharLiteral( Id, Node ); },
            [this, Id] ( const Frontend::BoolLiteral &Node ) -> llvm::Value *
            {
                llvm::Type *Shape = TypeOfExpr( Id );
                if ( Shape == nullptr or not Shape->isIntegerTy() )
                {
                    static_cast<void>( Fail( "llvm: the type claiming BoolLiteral has no integer layout" ) );
                    return nullptr;
                }
                return llvm::ConstantInt::get( Shape, Node.Value ? 1 : 0 );
            },
            [this, Id] ( const Frontend::NilLiteral & ) -> llvm::Value *
            {
                // Null of whatever shape the type claiming NilLiteral resolves
                // to — a `ptr` today, and nothing here depends on that.
                llvm::Type *Shape = TypeOfExpr( Id );
                if ( Shape == nullptr )
                {
                    static_cast<void>( Fail( "llvm: the type claiming NilLiteral has no resolved layout" ) );
                    return nullptr;
                }
                return llvm::Constant::getNullValue( Shape );
            },
            [this, Id] ( const Frontend::StringLiteral &Node ) -> llvm::Value * { return EmitStringLiteral( Id, Node ); },
            [this, Id] ( const Frontend::SymbolLiteral &Node ) -> llvm::Value *
            {
                // A symbol is an interned identity, and the *name* behind it
                // needs a runtime interner table — refused loudly upstream
                // (rules/core-ast.md), so only the identity is emitted here.
                llvm::Type *Shape = TypeOfExpr( Id );
                if ( Shape == nullptr or not Shape->isIntegerTy() )
                {
                    static_cast<void>( Fail( "llvm: the type claiming SymbolLiteral has no integer layout" ) );
                    return nullptr;
                }
                return llvm::ConstantInt::get( Shape, Node.Name.Value );
            },
            // Assign/Binary/Call nodes before this backend ever walks the tree.

            // --- Access ----------------------------------------------------
            [this, Id] ( const Frontend::Identifier & ) -> llvm::Value *
            {
                // `test_int8` — a paren-less, receiver-less call is a bare
                // `Identifier`, exactly as a paren-less `Member` is; only the
                // resolution tells it apart from a local/free-function-name
                // read. Sema records one for every member-ish node
                // (rules/core-ast.md); EmitResolvedCall already handles a
                // receiver-less entry (`Receiver` invalid, `Frame.Self` or no
                // owner at all for a module-level free function).
                if ( const Sema::CalleeEntry *Entry = Frame.Callees->Get( Id );
                     Entry != nullptr and Entry->Decl != nullptr and Entry->Decl->Kind == Sema::EMemberKind::Method )
                {
                    return EmitResolvedCall( Id, *Entry, Frontend::ExprId{}, {}, Frontend::ExprId{} );
                }
                return LoadPlace( Id );
            },
            [this, Id] ( const Frontend::InstanceVar & ) -> llvm::Value * { return LoadPlace( Id ); },
            [this, Id] ( const Frontend::Member &Node ) -> llvm::Value *
            {
                // `symbols.free` — a paren-less call is a bare `Member`, and
                // only the resolution tells it apart from a field read. Sema
                // records one for every member-ish node (rules/core-ast.md),
                // so that decision is read here, never re-derived.
                if ( const Sema::CalleeEntry *Entry = Frame.Callees->Get( Id );
                     Entry != nullptr and Entry->Decl != nullptr and Entry->Decl->Kind == Sema::EMemberKind::Method )
                {
                    return EmitResolvedCall( Id, *Entry, Node.Object, {}, Frontend::ExprId{} );
                }
                return LoadPlace( Id );
            },
            [this, Id] ( const Frontend::Deref & ) -> llvm::Value * { return LoadPlace( Id ); },
            [this] ( const Frontend::SelfExpr & ) -> llvm::Value *
            {
                if ( Frame.Self == nullptr )
                {
                    static_cast<void>( Fail( MissingSelf( "`self`" ) ) );
                }
                return Frame.Self;
            },
            // `super` is the same receiver: which body it reaches was decided
            // by Sema and travels in the call's CalleeEntry, not in the value.
            [this] ( const Frontend::SuperExpr & ) -> llvm::Value *
            {
                if ( Frame.Self == nullptr )
                {
                    static_cast<void>( Fail( MissingSelf( "`super`" ) ) );
                }
                return Frame.Self;
            },

            // --- Operations ------------------------------------------------
            [this, Id] ( const Frontend::Call &Node ) -> llvm::Value * { return EmitCall( Id, Node ); },
            [this] ( const Frontend::Assign &Node ) -> llvm::Value *
            {
                llvm::Value *Value = EmitExpr( Node.Value );
                llvm::Value *Place = EmitAddress( Node.Target );
                if ( Value == nullptr or Place == nullptr )
                {
                    return nullptr;
                }
                // AssignLowering (order 24) desugared every compound form, so
                // exactly one shape of Assign reaches here: a store.
                EmitStore( Place, Value, LayoutOfExpr( Node.Target ) );
                return Value;
            },
            [this, Id] ( const Frontend::Ternary &Node ) -> llvm::Value * { return EmitTernary( Id, Node ); },

            // --- Operators -------------------------------------------------
            [this, Id] ( const Frontend::Binary &Node ) -> llvm::Value * { return EmitBinary( Id, Node ); },
            [this, Id] ( const Frontend::Unary &Node ) -> llvm::Value * { return EmitUnary( Id, Node ); },

            // --- Control ---------------------------------------------------
            [this, Id] ( const Frontend::CaseExpr &Node ) -> llvm::Value * { return EmitCase( Id, Node ); },
            [this, Id] ( const Frontend::If &Node ) -> llvm::Value * { return EmitIf( Id, Node ); },
            [this, Id] ( const Frontend::BeginExpr &Node ) -> llvm::Value * { return EmitBegin( Id, Node ); },
            [this, Id] ( const Frontend::RaiseExpr &Node ) -> llvm::Value * { return EmitRaise( Id, Node ); },

            // --- Closures --------------------------------------------------
            [this, Id] ( const Frontend::Lambda &Node ) -> llvm::Value * { return EmitLambda( Id, Node ); },
            [this, Id] ( const Frontend::Block &Node ) -> llvm::Value * { return EmitBlock( Id, Node ); },

            // --- Inert -----------------------------------------------------
            // Neither carries a runtime value, and neither is descended into
            // (rules/core-ast.md). GenericInst is a *spelling* of a type in
            // value position — the Call wrapping it is what emits anything.
            [] ( const Frontend::GenericInst & ) -> llvm::Value * { return nullptr; },
            [this, Id] ( const Frontend::SizeOf & ) -> llvm::Value * { return EmitSizeOf( Id ); },

            // `( Value : Type )` — TypeChecker already constrained `Value` to
            // `Type` and stamped both with the same SemaTypeId (core-ast.md),
            // so by codegen it is a pure passthrough: no instruction of its
            // own, just `Value`'s.
            [this] ( const Frontend::TypedExpr &Node ) -> llvm::Value * { return EmitExpr( Node.Value ); },

            [this, Id] ( const auto & ) -> llvm::Value *
            {
                // Sugar, by elimination: AstInvariant (order 40) proves none
                // survives Lowering, so reaching this is a middle-end bug and
                // is named as one.
                static_cast<void>( Fail( "llvm: " + std::string( Frontend::NodeName( Frame.Unit->Ast->Expr( Id ) ) ) +
                                         " reached codegen — no Lowering pass removed it" ) );
                return nullptr;
            } },
        Ast.Expr( Id ) );
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::LoadPlace ( Frontend::ExprId Id )
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

    llvm::Type *Loaded = TypeOfLayout( Shape );
    if ( Loaded == nullptr )
    {
        static_cast<void>( Fail( "llvm: expression " + std::to_string( Id.Value ) + " has no resolved layout to load" ) );
        return nullptr;
    }
    return Builder->CreateLoad( Loaded, Address );
}

// ---------------------------------------------------------------------------
// Literals
// ---------------------------------------------------------------------------

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitIntLiteral ( Frontend::ExprId Id, const Frontend::IntLiteral &Node )
{
    // The width comes from the type that claimed IntLiteral through
    // `@[Literal]`, resolved by Sema onto this very expression — never from a
    // default the compiler picked.
    llvm::Type *Shape = TypeOfExpr( Id );
    if ( Shape == nullptr or not( Shape->isIntegerTy() or Shape->isFloatingPointTy() ) )
    {
        static_cast<void>(
            Fail( "llvm: integer literal '" + std::string( Frame.Unit->Ast->Text( Node.Raw ) ) + "' has no integer layout" ) );
        return nullptr;
    }

    std::uint64_t Value = 0;
    if ( not DecodeInteger( Frame.Unit->Ast->Text( Node.Raw ), Value ) )
    {
        static_cast<void>(
            Fail( "llvm: cannot decode integer literal '" + std::string( Frame.Unit->Ast->Text( Node.Raw ) ) + "'" ) );
        return nullptr;
    }

    // `self == 0` inside a generic `Arithmetic` default (`zero?`, `abs`, ...)
    // is unconstrained until instantiation, and adopts whatever `self`
    // turns out to be — a bare digit sequence is as much an unconstrained
    // literal as a `FloatLiteral` when the receiver is `Float32`/`Float64`
    // (TypeCheckerConstraint's `Expected` propagation types it there, never
    // this emitter), so the same node must be able to materialise either
    // constant kind.
    if ( Shape->isFloatingPointTy() )
    {
        return llvm::ConstantFP::get( Shape, static_cast<double>( Value ) );
    }
    return llvm::ConstantInt::get( Shape, Value );
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitFloatLiteral ( Frontend::ExprId Id, const Frontend::FloatLiteral &Node )
{
    llvm::Type *Shape = TypeOfExpr( Id );
    if ( Shape == nullptr or not Shape->isFloatingPointTy() )
    {
        static_cast<void>( Fail( "llvm: float literal '" + std::string( Frame.Unit->Ast->Text( Node.Raw ) ) +
                                 "' has no floating-point layout" ) );
        return nullptr;
    }

    double Value = 0.0;
    if ( not DecodeFloat( Frame.Unit->Ast->Text( Node.Raw ), Value ) )
    {
        static_cast<void>(
            Fail( "llvm: cannot decode float literal '" + std::string( Frame.Unit->Ast->Text( Node.Raw ) ) + "'" ) );
        return nullptr;
    }
    return llvm::ConstantFP::get( Shape, Value );
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitCharLiteral ( Frontend::ExprId Id, const Frontend::CharLiteral &Node )
{
    llvm::Type *Shape = TypeOfExpr( Id );
    if ( Shape == nullptr or not Shape->isIntegerTy() )
    {
        static_cast<void>( Fail( "llvm: the type claiming CharLiteral has no integer layout" ) );
        return nullptr;
    }

    std::uint64_t Value = 0;
    if ( not DecodeChar( Frame.Unit->Ast->Text( Node.Raw ), Value ) )
    {
        static_cast<void>(
            Fail( "llvm: cannot decode character literal '" + std::string( Frame.Unit->Ast->Text( Node.Raw ) ) + "'" ) );
        return nullptr;
    }
    return llvm::ConstantInt::get( Shape, Value );
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitStringLiteral ( Frontend::ExprId Id,
                                                                          const Frontend::StringLiteral &Node )
{
    // Escapes are resolved here, not in the lexer: the interned lexeme is the
    // source spelling, which is what `volt parse` and the golden fixtures show.
    // The decoded length is what the aggregate's size field must carry — `"a\n"`
    // is two bytes, not three.
    const std::string Text     = DecodeText( Frame.Unit->Ast->Text( Node.Value ) );
    const Sema::LayoutId Shape = LayoutOfExpr( Id );
    llvm::Type *Struct         = TypeOfLayout( Shape );
    if ( Struct == nullptr or not Struct->isStructTy() )
    {
        static_cast<void>( Fail( "llvm: the type claiming StringLiteral has no aggregate layout" ) );
        return nullptr;
    }

    // The bytes themselves: a private constant, deduplicated by the module.
    llvm::Constant *Bytes = Builder->CreateGlobalString( Text, ".str", 0, Mod.get() );

    // The aggregate is the stdlib's, and the emitter reads its *shape*, not its
    // field names: a pointer slot takes the bytes, an integer slot takes the
    // length. Anything else is a shape this emitter has no way to fill, and it
    // says so instead of writing into whichever field happened to be first.
    auto *Layout = llvm::cast<llvm::StructType>( Struct );
    std::vector<llvm::Constant *> Slots;
    Slots.reserve( Layout->getNumElements() );
    for ( unsigned Index = 0; Index < Layout->getNumElements(); ++Index )
    {
        llvm::Type *Slot = Layout->getElementType( Index );
        if ( Slot->isPointerTy() )
        {
            Slots.push_back( Bytes );
        }
        else if ( Slot->isIntegerTy() )
        {
            Slots.push_back( llvm::ConstantInt::get( Slot, Text.size() ) );
        }
        else
        {
            static_cast<void>( Fail( "llvm: the type claiming StringLiteral has a field this emitter cannot fill — "
                                     "a string literal is a pointer to bytes plus a length" ) );
            return nullptr;
        }
    }

    // The literal is a constant, so it lives in a private global rather than an
    // alloca: an aggregate is addressed (abi.md), and this address is valid for
    // the whole program.
    auto *Storage = new llvm::GlobalVariable( *Mod, Layout, true, llvm::GlobalValue::PrivateLinkage,
                                              llvm::ConstantStruct::get( Layout, Slots ), ".strlit" );
    Storage->setUnnamedAddr( llvm::GlobalValue::UnnamedAddr::Global );
    return Storage;
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitSizeOf ( Frontend::ExprId Id )
{
    // `sizeof T` names a type in a *TypeId*, which is a spelling — resolving
    // it here would be semantic analysis in codegen. TypeChecker publishes the
    // measured type on this node's own site instead, so all that is left is to
    // measure its layout, which is LayoutEngine's answer and nobody else's.
    const Sema::LayoutId Shape = LayoutOfValue( *Frame.Values, Frame.Values->SiteType( Sema::BindingSite{ Id } ) );
    if ( not Shape.IsValid() )
    {
        static_cast<void>( Fail( "llvm: `sizeof` at expression " + std::to_string( Id.Value ) +
                                 " — its operand names a type with no resolved layout" ) );
        return nullptr;
    }

    // The width is the one the *use site* gave the expression, exactly as for
    // an integer literal: `count * sizeof T` against a UInt64 count is a
    // UInt64 constant.
    llvm::Type *Width = TypeOfExpr( Id );
    if ( Width == nullptr or not Width->isIntegerTy() )
    {
        static_cast<void>( Fail( "llvm: `sizeof` at expression " + std::to_string( Id.Value ) + " has no integer layout" ) );
        return nullptr;
    }
    if ( not Layouts.has_value() )
    {
        static_cast<void>( Fail( "llvm: `sizeof` needs the layout engine, which the target was never initialised with" ) );
        return nullptr;
    }
    return llvm::ConstantInt::get( Width, Layouts->Of( Shape ).Size );
}

// ---------------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------------

// NOLINTBEGIN(clang-analyzer-security.ArrayBound) — false positive via LLVM's hung-off operand layout
llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitBinary ( Frontend::ExprId Id, const Frontend::Binary &Node )
{
    // The protocol, in the one order that keeps a backend free of member
    // lookup (rules/core-ast.md): the resolution first, the instruction only
    // when there is none.
    if ( const Sema::CalleeEntry *Entry = Frame.Callees->Get( Id );
         Entry != nullptr and Entry->Decl != nullptr and not Entry->Decl->bAbstract )
    {
        return EmitResolvedCall( Id, *Entry, Node.Lhs, std::span<const Frontend::ExprId>{ &Node.Rhs, 1 } );
    }

    // `and` / `or` are spelled operators that short-circuit, so they are
    // control flow and never a table row.
    if ( Node.Op == Frontend::TokenKind::KwAnd or Node.Op == Frontend::TokenKind::AndAnd or
         Node.Op == Frontend::TokenKind::KwOr or Node.Op == Frontend::TokenKind::OrOr )
    {
        return EmitShortCircuit( Node );
    }

    const Sema::LayoutId Shape      = LayoutOfExpr( Node.Lhs );
    const std::string_view Spelling = SpellingOf( Shape );
    const EOpFamily Family          = FamilyOf( Spelling );
    if ( Family == EOpFamily::None )
    {
        static_cast<void>( Fail( "llvm: operator '" + std::string( Frontend::TokenSpelling( Node.Op ) ) +
                                 "' is neither resolved to a method nor carried by a primitive receiver" ) );
        return nullptr;
    }

    // Pointer arithmetic is heterogeneous — `( offset : UInt64 ) -> Pointer<T>`
    // declared on the pointer nominal itself — so it is an address computation
    // with the pointee's stride, not an opcode.
    if ( Spelling == "ptr" and ( Node.Op == Frontend::TokenKind::Plus or Node.Op == Frontend::TokenKind::Minus ) )
    {
        return EmitPointerArith( Node );
    }

    llvm::Value *Lhs = EmitExpr( Node.Lhs );
    llvm::Value *Rhs = EmitExpr( Node.Rhs );
    if ( Lhs == nullptr or Rhs == nullptr )
    {
        return nullptr;
    }

    if ( const BinOpRow *Row = FindRow( std::span<const BinOpRow>{ BinOps }, Family, Node.Op ); Row != nullptr )
    {
        return Builder->CreateBinOp( Row->Opcode, Lhs, CoerceWidth( Rhs, Lhs->getType() ) );
    }
    if ( const CmpRow *Row = FindRow( std::span<const CmpRow>{ Cmps }, Family, Node.Op ); Row != nullptr )
    {
        return Family == EOpFamily::Float ? Builder->CreateFCmp( Row->Predicate, Lhs, Rhs )
                                          : Builder->CreateICmp( Row->Predicate, Lhs, CoerceWidth( Rhs, Lhs->getType() ) );
    }

    static_cast<void>( Fail( "llvm: no machine instruction for '" + std::string( Frontend::TokenSpelling( Node.Op ) ) +
                             "' on a '" + std::string( Spelling ) + "' receiver" ) );
    return nullptr;
}
// NOLINTEND(clang-analyzer-security.ArrayBound)

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitUnary ( Frontend::ExprId Id, const Frontend::Unary &Node )
{
    if ( const Sema::CalleeEntry *Entry = Frame.Callees->Get( Id );
         Entry != nullptr and Entry->Decl != nullptr and not Entry->Decl->bAbstract )
    {
        return EmitResolvedCall( Id, *Entry, Node.Operand, {} );
    }

    const std::string_view Spelling = SpellingOf( LayoutOfExpr( Node.Operand ) );
    const EOpFamily Family          = FamilyOf( Spelling );
    const UnOpRow *Row              = FindRow( std::span<const UnOpRow>{ UnOps }, Family, Node.Op );
    if ( Row == nullptr )
    {
        static_cast<void>( Fail( "llvm: no machine instruction for unary '" + std::string( Frontend::TokenSpelling( Node.Op ) ) +
                                 "' on a '" + std::string( Spelling ) + "' operand" ) );
        return nullptr;
    }

    llvm::Value *Operand = EmitExpr( Node.Operand );
    if ( Operand == nullptr )
    {
        return nullptr;
    }

    switch ( Row->Kind )
    {
    case EUnaryOp::Neg:
        return Builder->CreateNeg( Operand );
    case EUnaryOp::FNeg:
        return Builder->CreateFNeg( Operand );
    case EUnaryOp::BitNot:
        return Builder->CreateNot( Operand );
    case EUnaryOp::LogicalNot:
        // `xor true` on the one-bit shape, which is what `not` means; on a
        // wider integer the same row would silently mean something else, so it
        // is refused rather than reinterpreted.
        if ( not Operand->getType()->isIntegerTy( 1 ) )
        {
            static_cast<void>( Fail( "llvm: logical '" + std::string( Frontend::TokenSpelling( Node.Op ) ) + "' on a '" +
                                     std::string( Spelling ) + "' operand that is not one bit wide" ) );
            return nullptr;
        }
        return Builder->CreateXor( Operand, Builder->getTrue() );
    }
    return nullptr;
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitPointerArith ( const Frontend::Binary &Node )
{
    // The pointee is the first generic argument of whichever stdlib type
    // claimed PointerType — "Pointer" is not a name this compiler knows
    // (rules/core-ast.md).
    const Sema::UnitTypes &Values   = *Frame.Values;
    const Sema::SemaTypeId Receiver = Values.ExprType( Node.Lhs );
    if ( not Values.Has( Receiver ) or Values.Get( Receiver ).Args.Size() == 0 )
    {
        static_cast<void>( Fail( "llvm: pointer arithmetic on a receiver with no pointee argument" ) );
        return nullptr;
    }

    llvm::Type *Pointee = TypeOfLayout( LayoutOfValue( Values, Values.Get( Receiver ).Args[0] ) );
    if ( Pointee == nullptr )
    {
        static_cast<void>( Fail( "llvm: pointer arithmetic whose pointee has no resolved layout" ) );
        return nullptr;
    }

    llvm::Value *Base   = EmitExpr( Node.Lhs );
    llvm::Value *Offset = EmitExpr( Node.Rhs );
    if ( Base == nullptr or Offset == nullptr )
    {
        return nullptr;
    }

    Offset = CoerceWidth( Offset, Builder->getInt64Ty() );
    if ( Node.Op == Frontend::TokenKind::Minus )
    {
        Offset = Builder->CreateNeg( Offset );
    }
    return Builder->CreateGEP( Pointee, Base, Offset );
}

// NOLINTBEGIN(clang-analyzer-security.ArrayBound) — false positive via LLVM's hung-off operand layout
llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitShortCircuit ( const Frontend::Binary &Node )
{
    const bool bAnd = Node.Op == Frontend::TokenKind::KwAnd or Node.Op == Frontend::TokenKind::AndAnd;

    llvm::Value *Lhs = EmitExpr( Node.Lhs );
    if ( Lhs == nullptr )
    {
        return nullptr;
    }

    llvm::BasicBlock *Origin = Builder->GetInsertBlock();
    llvm::BasicBlock *Rest   = llvm::BasicBlock::Create( Context, bAnd ? "and.rhs" : "or.rhs", Frame.Fn );
    llvm::BasicBlock *Merge  = llvm::BasicBlock::Create( Context, bAnd ? "and.end" : "or.end", Frame.Fn );

    // `a and b` evaluates b only when a held; `a or b` only when it did not.
    static_cast<void>( bAnd ? Builder->CreateCondBr( Lhs, Rest, Merge ) : Builder->CreateCondBr( Lhs, Merge, Rest ) );

    Builder->SetInsertPoint( Rest );
    llvm::Value *Rhs = EmitExpr( Node.Rhs );
    if ( Rhs == nullptr )
    {
        return nullptr;
    }
    llvm::BasicBlock *RhsEnd = Builder->GetInsertBlock();
    static_cast<void>( Builder->CreateBr( Merge ) );

    Builder->SetInsertPoint( Merge );
    llvm::PHINode *Result = Builder->CreatePHI( Lhs->getType(), 2, "logic" );
    Result->addIncoming( bAnd ? Builder->getFalse() : Builder->getTrue(), Origin );
    Result->addIncoming( Rhs, RhsEnd );
    return Result;
}
// NOLINTEND(clang-analyzer-security.ArrayBound)

std::string_view Volt::Backend::Llvm::LlvmBackend::State::SpellingOf ( Sema::LayoutId Id ) const
{
    if ( not Id.IsValid() or Build == nullptr or Build->Types == nullptr )
    {
        return {};
    }
    const Sema::LayoutNode &Node = Build->Types->Get( Id );
    if ( const auto *Scalar = std::get_if<Sema::Primitive>( &Node ); Scalar != nullptr and Scalar->Spelling.IsValid() )
    {
        return Build->Types->Text( Scalar->Spelling );
    }
    // A Pointer layout is an address just as `@[Primitive("ptr")]` is, and the
    // two must select the same instructions.
    return std::holds_alternative<Sema::Pointer>( Node ) ? std::string_view{ "ptr" } : std::string_view{};
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::CoerceWidth ( llvm::Value *Value, llvm::Type *Target ) const
{
    // TypeCompat's IsWideningScalar accepts an `i8` where a `u64` is expected
    // (rules/zero-hardcode.md: Volt has no integer conversion at all, so `hash`
    // would be unwritable without it). That decision was Sema's; here it is
    // simply honoured with the zext the machine needs. Signedness deliberately
    // does not enter the rule, and a zext is what "same family, never
    // narrowing" means on the wire.
    if ( Value == nullptr or Target == nullptr or Value->getType() == Target )
    {
        return Value;
    }
    if ( Value->getType()->isIntegerTy() and Target->isIntegerTy() and
         Value->getType()->getIntegerBitWidth() < Target->getIntegerBitWidth() )
    {
        return Builder->CreateZExt( Value, Target );
    }
    // An aggregate expression evaluates to a `ptr` at its storage — the
    // convention this whole file follows — but a *result* crosses a frame
    // boundary, and the storage it points at is the callee's own. So the one
    // place the value has to leave its slot is a `ret` of an aggregate, where
    // the target type is the struct itself: load it. Nothing else in this
    // emitter ever asks for a struct-typed target, since ParamTypeOfLayout
    // turns every aggregate *parameter* into a `ptr`.
    if ( Target->isStructTy() and Value->getType()->isPointerTy() )
    {
        return Builder->CreateLoad( Target, Value );
    }
    return Value;
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitDefaultArgument ( const Sema::Member &Decl, std::size_t Index )
{
    const UnitView *Home = nullptr;
    for ( const UnitView &View : Build->Units )
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

    // The default belongs to the *declaring* unit: its type, and any callee it
    // resolves, live in that unit's tables and nowhere else. Only those three
    // move — the slots, `self` and the block stay this frame's, because a
    // default that reached for one of them would be reaching into a frame that
    // does not exist yet, and the loud failure that follows is the right one.
    const UnitView *SavedUnit             = Frame.Unit;
    const Sema::UnitTypes *SavedValues    = Frame.Values;
    const Sema::UnitCallees *SavedCallees = Frame.Callees;
    Frame.Unit                            = Home;
    Frame.Values                          = Home->Values;
    Frame.Callees                         = Home->Callees;

    llvm::Value *Value = EmitExpr( Default );

    Frame.Unit    = SavedUnit;
    Frame.Values  = SavedValues;
    Frame.Callees = SavedCallees;
    return Value;
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitCall ( Frontend::ExprId Id, const Frontend::Call &Node )
{
    const Sema::CalleeEntry *Entry = Frame.Callees->Get( Node.Callee );
    if ( Entry == nullptr or Entry->Decl == nullptr )
    {
        static_cast<void>( Fail( "llvm: call at expression " + std::to_string( Id.Value ) +
                                 " carries no callee resolution — TypeChecker records one for every call it accepts" ) );
        return nullptr;
    }

    // The receiver is the callee's own object, when the callee is a member
    // access. A free function's callee is an Identifier and has none — unless
    // the resolution is indirect, where the callee expression *is* the
    // callable being invoked: `f( x )` on a local holding a closure.
    Frontend::ExprId Receiver;
    if ( const auto *Access = std::get_if<Frontend::Member>( &Frame.Unit->Ast->Expr( Node.Callee ) ); Access != nullptr )
    {
        Receiver = Access->Object;
    }
    else if ( Entry->bIndirect )
    {
        Receiver = Node.Callee;
    }

    return EmitResolvedCall( Id, *Entry, Receiver, std::span<const Frontend::ExprId>{ Node.Args.begin(), Node.Args.Size() },
                             Node.BlockArg );
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitResolvedCall ( Frontend::ExprId Id,
                                                                         const Sema::CalleeEntry &Entry,
                                                                         Frontend::ExprId Receiver,
                                                                         std::span<const Frontend::ExprId> Args,
                                                                         Frontend::ExprId Block )
{
    // An indirect callee has no body and no symbol: the callable being
    // invoked is the receiver, and the call goes through its `{ code, env }`
    // pair rather than to a mangled name. Sema decided this (CalleeEntry::
    // bIndirect); nothing is re-derived here.
    if ( Entry.bIndirect )
    {
        if ( Block.IsValid() )
        {
            static_cast<void>( Fail( "llvm: the call at expression " + std::to_string( Id.Value ) +
                                     " passes a trailing block to a callable, whose type carries positional arguments only" ) );
            return nullptr;
        }
        return EmitIndirectCall( Id, Entry, Receiver, Args );
    }

    const Sema::UnitTypes &Values = *Frame.Values;

    // The owner and the instantiation both come out of the entry Sema recorded:
    // NominalIds are the cross-unit currency, and the flattened bindings are
    // the same encoding the mangler and the layout cache key on.
    Sema::NominalId Owner;
    if ( Values.Has( Entry.Receiver ) )
    {
        Owner = Values.Get( Entry.Receiver ).Base;
    }

    // A member `Owner` does not itself own (found on some ancestor or mixin
    // instead — LookupMember's own doc: "the MemberRef it hands back carries
    // the declaring nominal, not an instantiation") has no body defined
    // against *this* receiver: DefineAll only ever defines a member under the
    // NominalId that owns it in `Store.Type( Id ).Members`, never under every
    // type that merely inherits it. A mixin's own default (`Arithmetic#min`)
    // is additionally never defined at all — IsMixinOwner excludes it from
    // both sweeps, since its `self` means "whichever type includes me" and
    // has no signature until one does. This is exactly what a generic body
    // is: unresolved until a call site fixes it. So an inherited default is
    // routed through the same Monomorphizer queue below, keyed on the
    // *receiver's* Owner+Name — MangleFunction already mangles the receiver,
    // not the declaring mixin, so `Int32.min`/`Float64.min` land on distinct
    // symbols even though both share one AST body.
    bool bOwnMember = false;
    if ( Owner.IsValid() )
    {
        for ( const Sema::Member &Candidate : Build->Types->Type( Owner ).Members )
        {
            if ( &Candidate == Entry.Decl )
            {
                bOwnMember = true;
                break;
            }
        }
    }
    const bool bInherited = Owner.IsValid() and not bOwnMember;

    // `Entry.Bindings` is the *declaring* type's parameter space, then the
    // method's own generics. For an inherited default that declaring type is
    // the mixin, whose space is empty — so taking it verbatim would erase the
    // receiver's arguments, and `Pointer<UInt8>#!=` would instantiate with no
    // `T` at all: its body's `self <=> other` would then resolve on a
    // `Pointer` with no argument, mangle to a symbol nothing defines, and the
    // failure would surface as an undefined symbol at link. The request is
    // keyed on the receiver (MangleFunction mangles the receiver, not the
    // declaring mixin), so the receiver's own arguments are what must reach
    // it; the method's own generic slots still come from the resolution.
    std::vector<std::uint32_t> FlatArgs;
    if ( bInherited and Values.Has( Entry.Receiver ) )
    {
        for ( const Sema::SemaTypeId Arg : Values.Get( Entry.Receiver ).Args )
        {
            FlattenValueType( Values, Arg, FlatArgs );
        }
        const std::size_t Own   = std::min<std::size_t>( Entry.Decl->OwnGenerics, Entry.Bindings.Size() );
        const std::size_t First = Entry.Bindings.Size() - Own;
        for ( std::size_t Index = First; Index < Entry.Bindings.Size(); ++Index )
        {
            FlattenValueType( Values, Entry.Bindings[Index], FlatArgs );
        }
    }
    else
    {
        for ( const Sema::SemaTypeId Binding : Entry.Bindings )
        {
            FlattenValueType( Values, Binding, FlatArgs );
        }
    }

    // A generic owner or a method with its own generics has no body yet —
    // DeclareAll/DefineAll both skip it outright, since neither sweep knows
    // the arguments this call site just fixed. FunctionFor below still
    // synthesises its *declaration* on demand (so a forward or mutually
    // recursive call resolves), but the body is Monomorphizer's job:
    // enqueue is idempotent (MonoRequest::Key dedupes), so every call site
    // reaching the same instantiation is free to ask again. An inherited
    // default is enqueued unconditionally on FlatArgs — the receiver alone
    // (`Owner`) is enough to key the request even when neither the receiver
    // nor the method itself is generic.
    const bool bGenericOwner = Owner.IsValid() and Build->Types->Type( Owner ).Params.Size() > 0;
    if ( ( ( bGenericOwner or Entry.Decl->OwnGenerics > 0 ) and not FlatArgs.empty() ) or bInherited )
    {
        Mono.Enqueue( MonoRequest{ .Owner = Owner, .Name = Entry.Decl->Name, .Args = FlatArgs } );
    }

    llvm::Function *Callee = FunctionFor( *Entry.Decl, Owner, FlatArgs );
    if ( Callee == nullptr )
    {
        return nullptr;
    }

    std::vector<llvm::Value *> Actuals;
    Actuals.reserve( Args.size() + 1 );

    // abi.md's order, and the same one the declare sweep built the signature
    // with: `self` first, then the declared parameters.
    const bool bExternal = Entry.Decl->ExternSymbol.IsValid();

    // `T.new( … )` (Sema's CalleeEntry::bConstructs): the receiver expression
    // names a *type*, so there is nothing to evaluate for it — the storage the
    // initializer writes into has to come from this call, and that storage is
    // what the whole expression evaluates to, not the initializer's own result.
    llvm::Value *Constructed = nullptr;
    if ( Owner.IsValid() and not Entry.Decl->bSelf and not bExternal )
    {
        llvm::Value *Self = nullptr;
        if ( Entry.bConstructs )
        {
            const Sema::LayoutId Shape = LayoutOfValue( Values, Entry.Result );
            llvm::Type *Instance       = TypeOfLayout( Shape );
            if ( Instance == nullptr or not IsAggregate( Shape ) )
            {
                // A non-aggregate `self` is passed by value (abi.md), so an
                // initializer could not write through it. Nothing in the
                // stdlib constructs one; saying so beats a silent no-op.
                static_cast<void>( Fail( "llvm: the value constructed at expression " + std::to_string( Id.Value ) +
                                         " has no aggregate layout to initialise" ) );
                return nullptr;
            }
            Constructed = MakeTemp( Instance, "new" );
            Self        = Constructed;
        }
        else if ( Receiver.IsValid() )
        {
            Self = EmitExpr( Receiver );
        }
        else if ( Frame.Self != nullptr )
        {
            // A bare `capture_backtrace()` inside a method body: Sema resolved
            // the name on `self` (ExprInferencer's Identifier case, "inside a
            // method body a bare name is a member of self"), so the callee is
            // an Identifier with no object to evaluate and the receiver is
            // this frame's own.
            Self = Frame.Self;
        }
        else
        {
            static_cast<void>(
                Fail( "llvm: instance call at expression " + std::to_string( Id.Value ) + " has no receiver expression" ) );
            return nullptr;
        }

        if ( Self == nullptr )
        {
            return nullptr;
        }
        Actuals.push_back( Self );
    }

    // The declared parameters, in their own order — which is not the argument
    // list's: a `&block` slot binds through the call's trailing `do ... end`
    // and is skipped by positional matching (TypeStore::Member), so the two are
    // walked together rather than one being assumed to be the other.
    std::size_t Positional = 0;
    bool bBlockBound       = false;
    for ( std::size_t Index = 0; Index < Entry.Decl->Params.Size(); ++Index )
    {
        const bool bBlockSlot = Index < Entry.Decl->ParamIsBlock.Size() and Entry.Decl->ParamIsBlock[Index];

        Frontend::ExprId Arg;
        if ( bBlockSlot )
        {
            Arg         = Block;
            bBlockBound = true;
        }
        else if ( Positional < Args.size() )
        {
            Arg = Args[Positional];
            ++Positional;
        }

        // A parameter the call omits is filled from its declared default. No
        // pass materialises that into the argument list — one that did would
        // have to run after TypeChecker and type what it creates, which
        // rules/core-ast.md forbids — so the expression is emitted here, in
        // the unit that declares it, where TypeChecker already typed it
        // (EnterMethod infers and constrains every Param::Default). Nothing is
        // decided; a recorded, already-typed expression is read.
        llvm::Value *Value = Arg.IsValid() ? EmitExpr( Arg ) : EmitDefaultArgument( *Entry.Decl, Index );
        if ( Value == nullptr )
        {
            if ( not Arg.IsValid() and not Failed() )
            {
                static_cast<void>( Fail( "llvm: the call at expression " + std::to_string( Id.Value ) +
                                         " supplies no argument for parameter " + std::to_string( Index ) + " of '" +
                                         std::string( Build->Types->Text( Entry.Decl->Name ) ) +
                                         "', which declares no default" ) );
            }
            return nullptr;
        }
        Actuals.push_back( Value );
    }

    if ( Block.IsValid() and not bBlockBound )
    {
        static_cast<void>( Fail( "llvm: the call at expression " + std::to_string( Id.Value ) +
                                 " carries a trailing block, and '" + std::string( Build->Types->Text( Entry.Decl->Name ) ) +
                                 "' declares no `&block` parameter to bind it to" ) );
        return nullptr;
    }

    llvm::FunctionType *Signature = Callee->getFunctionType();
    if ( Actuals.size() != Signature->getNumParams() )
    {
        static_cast<void>( Fail( "llvm: call at expression " + std::to_string( Id.Value ) + " passes " +
                                 std::to_string( Actuals.size() ) + " arguments to a signature taking " +
                                 std::to_string( Signature->getNumParams() ) ) );
        return nullptr;
    }
    for ( std::size_t Index = 0; Index < Actuals.size(); ++Index )
    {
        Actuals[Index] = CoerceWidth( Actuals[Index], Signature->getParamType( static_cast<unsigned>( Index ) ) );
    }

    llvm::Value *Result = Builder->CreateCall( Callee, Actuals );

    // The mirror of CoerceWidth's aggregate case: a struct comes back by
    // value, and every consumer here expects an aggregate to *be* an address.
    // Spilling it into this frame's own slot is also what makes the result
    // outlive the callee's storage, which is the whole reason it was returned
    // by value rather than as a pointer.
    if ( Result->getType()->isStructTy() )
    {
        llvm::Value *Slot = MakeTemp( Result->getType(), "call.result" );
        if ( Slot == nullptr )
        {
            static_cast<void>(
                Fail( "llvm: no frame to hold the aggregate result of the call at expression " + std::to_string( Id.Value ) ) );
            return nullptr;
        }
        static_cast<void>( Builder->CreateStore( Result, Slot ) );
        Result = Slot;
    }

    // An `@[External]` symbol is C, and C cannot raise a Volt exception — the
    // thread-local state it might happen to read was some *other* Volt call's,
    // never its own, so the post-call check only runs for Volt code.
    if ( not bExternal )
    {
        EmitExceptionCheck();
    }
    return Constructed != nullptr ? Constructed : Result;
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

// NOLINTBEGIN(clang-analyzer-security.ArrayBound) — false positive via LLVM's hung-off operand layout
llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitTernary ( Frontend::ExprId Id, const Frontend::Ternary &Node )
{
    llvm::Value *Cond = EmitExpr( Node.Cond );
    if ( Cond == nullptr )
    {
        return nullptr;
    }

    // Two blocks and a phi, never a `select`: both arms are arbitrary
    // expressions, and `select` would evaluate the one not taken.
    llvm::BasicBlock *ThenBlock = llvm::BasicBlock::Create( Context, "ternary.then", Frame.Fn );
    llvm::BasicBlock *ElseBlock = llvm::BasicBlock::Create( Context, "ternary.else", Frame.Fn );
    llvm::BasicBlock *Merge     = llvm::BasicBlock::Create( Context, "ternary.end", Frame.Fn );
    static_cast<void>( Builder->CreateCondBr( Cond, ThenBlock, ElseBlock ) );

    Builder->SetInsertPoint( ThenBlock );
    llvm::Value *Then = EmitExpr( Node.Then );
    if ( Then == nullptr )
    {
        return nullptr;
    }
    llvm::BasicBlock *ThenEnd = Builder->GetInsertBlock();
    static_cast<void>( Builder->CreateBr( Merge ) );

    Builder->SetInsertPoint( ElseBlock );
    llvm::Value *Else = EmitExpr( Node.Else );
    if ( Else == nullptr )
    {
        return nullptr;
    }
    llvm::BasicBlock *ElseEnd = Builder->GetInsertBlock();
    static_cast<void>( Builder->CreateBr( Merge ) );

    Builder->SetInsertPoint( Merge );
    // An aggregate arm evaluates to a `ptr` at its storage, never to the
    // struct value itself (the ABI convention this whole file follows) — so
    // the PHI must merge on `ptr`, exactly what ParamTypeOfLayout already
    // computes for a by-pointer parameter. Using TypeOfExpr's struct type
    // here instead crashes CreatePHI/addIncoming the moment either arm is a
    // `String`/user-struct value (`self ? "true" : "false"`), since Then/Else
    // are pointers but the PHI's declared type would not be.
    llvm::Type *Shape = ParamTypeOfLayout( LayoutOfExpr( Id ) );
    if ( Shape == nullptr )
    {
        Shape = Then->getType();
    }
    // CoerceWidth reconciles two integer widths and nothing else, so an arm
    // that is a different *kind* of value (an integer against a pointer, a
    // float against an integer) survives it unchanged. Merging those into one
    // PHI is an assertion failure inside LLVM, i.e. a crash on well-typed Volt
    // — which is the one outcome this backend must never produce. Refuse by
    // name instead: it means the two arms were given types that do not meet,
    // and that is a middle-end answer, not something to repair here.
    llvm::Value *ThenValue = CoerceWidth( Then, Shape );
    llvm::Value *ElseValue = CoerceWidth( Else, Shape );
    if ( ThenValue->getType() != Shape or ElseValue->getType() != Shape )
    {
        std::string Report;
        llvm::raw_string_ostream Out{ Report };
        Out << "llvm: the arms of the ternary at expression " << Id.Value << " have types that do not meet — `then` is ";
        ThenValue->getType()->print( Out );
        Out << ", `else` is ";
        ElseValue->getType()->print( Out );
        Out << ", and the expression's own is ";
        Shape->print( Out );
        static_cast<void>( Fail( Report ) );
        return nullptr;
    }

    llvm::PHINode *Result = Builder->CreatePHI( Shape, 2, "ternary" );
    Result->addIncoming( ThenValue, ThenEnd );
    Result->addIncoming( ElseValue, ElseEnd );
    return Result;
}
// NOLINTEND(clang-analyzer-security.ArrayBound)

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitCase ( Frontend::ExprId Id, const Frontend::CaseExpr &Node )
{
    // After CaseLowering (order 22) this is a flat list of
    // `WhenClause{ Patterns: [expr Bool], Body }` — the pattern *is* the test,
    // and the scrutinee has already been folded into it. So the emission is a
    // conditional ladder and nothing more; that flatness is exactly why
    // CaseExpr stayed core (rules/core-ast.md).
    if ( Node.Target.IsValid() )
    {
        static_cast<void>( Fail( "llvm: a `case` still carrying its target reached codegen — CaseLowering folds it "
                                 "into each clause's patterns" ) );
        return nullptr;
    }

    const Frontend::AstContext &Ast = *Frame.Unit->Ast;
    llvm::BasicBlock *Merge         = llvm::BasicBlock::Create( Context, "case.end", Frame.Fn );

    // A `case` in value position needs a slot: its clauses are statement lists,
    // so their results converge through storage rather than a phi over blocks
    // whose count is not known until the ladder is built. An aggregate result
    // gets a slot too — `StoreTailValue` converges it by `EmitStore`'s memcpy,
    // not a plain `CreateStore` of an SSA struct value.
    llvm::Type *Shape           = TypeOfExpr( Id );
    const Sema::LayoutId Layout = LayoutOfExpr( Id );
    llvm::AllocaInst *Slot      = nullptr;
    if ( Shape != nullptr )
    {
        Slot = MakeTemp( Shape, "case.result" );
    }

    const auto EmitBody = [&] ( const Frontend::StmtList &Body )
    {
        // The clause's value is its trailing expression, stored rather than
        // returned — the same tail rule the function body uses.
        for ( std::size_t Index = 0; Index < Body.Size() and not Terminated(); ++Index )
        {
            const bool bLast = Index + 1 == Body.Size();
            if ( bLast and Slot != nullptr )
            {
                if ( const auto *Tail = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Body[Index] ) ); Tail != nullptr )
                {
                    StoreTailValue( EmitExpr( Tail->Expr ), Slot, Shape, Layout );
                    continue;
                }
            }
            EmitStmt( Body[Index], false );
        }
        if ( not Terminated() )
        {
            static_cast<void>( Builder->CreateBr( Merge ) );
        }
    };

    for ( const Frontend::StmtId ClauseId : Node.Clauses )
    {
        const auto *Clause = std::get_if<Frontend::WhenClause>( &Ast.Stmt( ClauseId ) );
        if ( Clause == nullptr )
        {
            static_cast<void>( Fail( "llvm: a `case` clause is not a WhenClause" ) );
            return nullptr;
        }

        llvm::BasicBlock *Body = llvm::BasicBlock::Create( Context, "when.body", Frame.Fn );
        llvm::BasicBlock *Next = llvm::BasicBlock::Create( Context, "when.next", Frame.Fn );

        // Several patterns on one clause are alternatives: any match enters the
        // body, so each falls through to the next test rather than to `Next`.
        for ( std::size_t Index = 0; Index < Clause->Patterns.Size(); ++Index )
        {
            llvm::Value *Test = EmitExpr( Clause->Patterns[Index] );
            if ( Test == nullptr )
            {
                return nullptr;
            }
            const bool bLast         = Index + 1 == Clause->Patterns.Size();
            llvm::BasicBlock *Missed = bLast ? Next : llvm::BasicBlock::Create( Context, "when.alt", Frame.Fn );
            static_cast<void>( Builder->CreateCondBr( Test, Body, Missed ) );
            Builder->SetInsertPoint( Missed );
        }
        if ( Clause->Patterns.Size() == 0 )
        {
            static_cast<void>( Builder->CreateBr( Next ) );
            Builder->SetInsertPoint( Next );
        }

        llvm::BasicBlock *Resume = Builder->GetInsertBlock();
        Builder->SetInsertPoint( Body );
        EmitBody( Clause->Body );
        Builder->SetInsertPoint( Resume );

        if ( Failed() )
        {
            return nullptr;
        }
    }

    EmitBody( Node.ElseBody );

    Builder->SetInsertPoint( Merge );
    if ( Slot == nullptr )
    {
        return nullptr;
    }
    return LoadConverged( Slot, Shape, Layout, "case" );
}

llvm::Value *Volt::Backend::Llvm::LlvmBackend::State::EmitIf ( Frontend::ExprId Id, const Frontend::If &Node )
{
    // An `if` converges the same way a `case` does, and for the same reason:
    // a branch is a *statement list*, so the number of blocks reaching the
    // merge is only known once the whole chain has been emitted — which rules
    // out a phi. A slot plus StoreTailValue handles that, and handles an
    // aggregate result (a branch producing a String) by memcpy rather than by
    // storing an SSA struct.
    llvm::Value *Cond = EmitExpr( Node.Cond );
    if ( Cond == nullptr )
    {
        return nullptr;
    }

    const Frontend::AstContext &Ast = *Frame.Unit->Ast;
    const bool bHasElse             = Node.Else.Size() > 0;

    llvm::BasicBlock *Then  = llvm::BasicBlock::Create( Context, "if.then", Frame.Fn );
    llvm::BasicBlock *Else  = bHasElse ? llvm::BasicBlock::Create( Context, "if.else", Frame.Fn ) : nullptr;
    llvm::BasicBlock *Merge = llvm::BasicBlock::Create( Context, "if.end", Frame.Fn );
    static_cast<void>( Builder->CreateCondBr( Cond, Then, bHasElse ? Else : Merge ) );

    // No shape means the `if` is in statement position (or a `-> Void` tail):
    // there is nothing to converge, and the branches are emitted for effect.
    llvm::Type *Shape           = TypeOfExpr( Id );
    const Sema::LayoutId Layout = LayoutOfExpr( Id );
    llvm::AllocaInst *Slot      = Shape != nullptr ? MakeTemp( Shape, "if.result" ) : nullptr;

    const auto EmitBranch = [&] ( const Frontend::StmtList &Body )
    {
        for ( std::size_t Index = 0; Index < Body.Size() and not Terminated(); ++Index )
        {
            const bool bLast = Index + 1 == Body.Size();
            if ( bLast and Slot != nullptr )
            {
                if ( const auto *Tail = std::get_if<Frontend::ExprStmt>( &Ast.Stmt( Body[Index] ) ); Tail != nullptr )
                {
                    StoreTailValue( EmitExpr( Tail->Expr ), Slot, Shape, Layout );
                    continue;
                }
            }
            EmitStmt( Body[Index], false );
        }
        if ( not Terminated() )
        {
            static_cast<void>( Builder->CreateBr( Merge ) );
        }
    };

    // An `elsif` chain is a nested If inside Else (Expr.hpp), so it is emitted
    // by the recursive call this branch makes through EmitStmt — the chain
    // needs no special case of its own.
    Builder->SetInsertPoint( Then );
    EmitBranch( Node.Then );

    if ( bHasElse )
    {
        Builder->SetInsertPoint( Else );
        EmitBranch( Node.Else );
    }

    Builder->SetInsertPoint( Merge );
    if ( Slot == nullptr )
    {
        return nullptr;
    }
    return LoadConverged( Slot, Shape, Layout, "if" );
}
