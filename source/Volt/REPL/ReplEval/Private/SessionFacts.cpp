// SessionFacts.cpp — everything a session can be *asked*, as data.
//
// The other half of this module compiles lines and runs them. This half never
// runs anything the user did not ask to run, and that distinction is the whole
// design of it:
//
//   - `:type` and `:ir` compile an expression and abandon the emission. No
//     generation is opened, so nothing can be left behind to leak.
//   - `:bench` is the one query that executes, and it executes inside a
//     generation it drops before returning.
//   - everything else — `:layout`, `:src`, `:doc`, completion — is a read of
//     the type store or of the source text, and compiles nothing at all.
//
// Nothing here formats. A layout is a list of fields with numbers on them; who
// draws the box around it is ReplQuery's business, and whether the box is
// coloured is ReplTui's.

#include "Volt/ReplEval/Evaluator.hpp"

#include "EvaluatorState.hpp"

#include "Volt/BackendCore/LayoutEngine.hpp"
#include "Volt/BackendCore/Mangler.hpp"
#include "Volt/Driver/Driver.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/MiddleEnd/TypeSystem/TypeUniverse.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

using namespace Volt;

// A name as the user wrote it, split into the type it qualifies and the member
// it names. `Array.map`, `Array#map` and `map` are the three shapes.
struct SplitName
{

    std::string_view Owner;
    std::string_view Member;
};

[[nodiscard]] SplitName Split ( const std::string_view Name )
{
    const std::size_t Cut = Name.find_last_of( ".#" );
    if ( Cut == std::string_view::npos )
    {
        return SplitName{ .Owner = {}, .Member = Name };
    }
    return SplitName{ .Owner = Name.substr( 0, Cut ), .Member = Name.substr( Cut + 1 ) };
}

[[nodiscard]] std::string_view Trim ( std::string_view Text )
{
    while ( not Text.empty() and ( Text.front() == ' ' or Text.front() == '\t' or Text.front() == '\n' or Text.front() == '\r' ) )
    {
        Text.remove_prefix( 1 );
    }
    while ( not Text.empty() and ( Text.back() == ' ' or Text.back() == '\t' or Text.back() == '\n' or Text.back() == '\r' ) )
    {
        Text.remove_suffix( 1 );
    }
    return Text;
}

} // namespace

// --- Scratch units ------------------------------------------------------------

Volt::Repl::Evaluator::State::Scratch Volt::Repl::Evaluator::State::Analyze ( const std::string_view Text, const bool bBind )
{
    Scratch Out;

    std::string Source( Text );
    if ( Source.empty() or Source.back() != '\n' )
    {
        Source += '\n';
    }

    const std::size_t Serial = Lines;
    const std::string Label  = "<repl:" + std::to_string( Serial + 1 ) + ">";

    const std::unordered_set<std::string> Mentioned = IdentifiersIn( Source );

    const Driver::Driver::AppendedUnit Appended = TheDriver.AppendUnit( Label, Source );
    Out.Index                                   = Appended.Index;
    ++Lines;

    {
        Frontend::AstContext &Ast = TheDriver.UnitAt( Out.Index ).Ast;
        if ( bBind )
        {
            Out.Bound = BindResult( Ast, Serial );
        }
        NameForeignStorage( Ast, Mentioned );
    }

    const Driver::Driver::UnitResult Unit = TheDriver.AnalyzeUnit( Out.Index, Appended.DiagMark );
    {
        std::ostringstream Report;
        TheDriver.ConsumeLineDiagnostics( Unit.DiagMark, Report );
        Out.Diagnostics = Report.str();
    }

    Out.bOk     = Unit.bOk;
    Out.Ordinal = Unit.Ordinal;
    return Out;
}

Volt::MiddleEnd::TypeSystem::SemaTypeId Volt::Repl::Evaluator::State::TypeOfBinding ( const Scratch &Unit ) const
{
    if ( not Unit.bOk or Unit.Bound.empty() or Unit.Ordinal >= TheDriver.UnitCount() )
    {
        return {};
    }

    // The root scope, not the AST: an implicit `x = 5` has no LocalDecl at all,
    // and reading the binding is what makes the two spellings answer alike.
    const Driver::CompileUnit &Compiled = TheDriver.Unit( Unit.Ordinal );
    if ( Compiled.Scopes.Size() == 0 )
    {
        return {};
    }

    const MiddleEnd::Resolver::Scope &Top = Compiled.Scopes.Get( MiddleEnd::Resolver::ScopeId{ 0 } );
    for ( const auto &[Name, Binding] : Top.Bindings )
    {
        if ( Compiled.Ast.Text( Name ) == Unit.Bound )
        {
            return Compiled.Types.SiteType( Binding.Site );
        }
    }
    return {};
}

// --- Naming a declaration -------------------------------------------------------

Volt::Repl::Evaluator::State::Found Volt::Repl::Evaluator::State::Resolve ( const std::string_view Name ) const
{
    using namespace MiddleEnd::TypeSystem;

    Found Out;

    const std::string_view Wanted = Trim( Name );
    if ( Wanted.empty() )
    {
        Out.Message = "a name is needed";
        return Out;
    }

    const TypeStore &Store = TheDriver.Layouts();
    const SplitName Parts  = Split( Wanted );

    if ( Parts.Owner.empty() )
    {
        // A bare name is a type, a free function, or nothing.
        if ( const std::optional<NominalId> Type = Store.LookupType( Parts.Member ) )
        {
            Out.bOk   = true;
            Out.bType = true;
            Out.Owner = *Type;
            return Out;
        }
        if ( const Member *Function = Store.LookupFunction( Parts.Member ); Function != nullptr )
        {
            Out.bOk   = true;
            Out.Entry = Function;
            return Out;
        }

        Out.Message = "'" + std::string( Parts.Member ) + "' is not a type or a function this session knows";
        return Out;
    }

    const std::optional<NominalId> Owner = Store.LookupType( Parts.Owner );
    if ( not Owner )
    {
        Out.Message = "'" + std::string( Parts.Owner ) + "' is not a type this session knows";
        return Out;
    }

    // Own body, then mixins, then the superclass — the order a call would
    // resolve in, so what is shown is what would run.
    const TypeStore::MemberRef Entry = Store.LookupMember( *Owner, Parts.Member );
    if ( Entry.Decl == nullptr )
    {
        Out.Message = "'" + std::string( Parts.Owner ) + "' has no member '" + std::string( Parts.Member ) + "'";
        return Out;
    }

    Out.bOk   = true;
    Out.Owner = Entry.Owner;
    Out.Entry = Entry.Decl;
    return Out;
}

Volt::Core::SourceRange Volt::Repl::Evaluator::State::RangeOf ( const std::uint32_t Unit, const Frontend::DeclId Decl ) const
{
    if ( not Decl.IsValid() or Unit >= TheDriver.UnitCount() )
    {
        return {};
    }

    const Driver::CompileUnit &Compiled = TheDriver.Unit( Unit );
    if ( Decl.Value >= Compiled.Ast.DeclCount() )
    {
        return {};
    }
    return Frontend::LocOf( Compiled.Ast.Decl( Decl ) );
}

std::string Volt::Repl::Evaluator::State::WhyNoSource ( const std::uint32_t Unit ) const
{
    if ( Unit >= TheDriver.UnitCount() )
    {
        return "it was not declared in this session";
    }

    const Driver::CompileUnit &Compiled = TheDriver.Unit( Unit );
    if ( Compiled.Ast.DeclCount() == 0 and Unit < TheDriver.StdlibUnitCount() )
    {
        return "the stdlib was loaded from its frontend cache, which holds signatures rather than syntax\n"
               "       -> start the session with --no-stdlib-cache to read stdlib source";
    }
    return "there is no source range recorded for it";
}

std::string Volt::Repl::Evaluator::State::DescribeSig ( const MiddleEnd::TypeSystem::SigTypeId Id,
                                                        const MiddleEnd::TypeSystem::NominalId Owner,
                                                        const std::uint32_t Depth ) const
{
    using namespace MiddleEnd::TypeSystem;

    const TypeStore &Store = TheDriver.Layouts();
    if ( not Id.IsValid() or Depth > 8 )
    {
        return "?";
    }

    const SigType &Value = Store.Sig( Id );
    if ( Value.ParamIndex == SigType::SelfParam )
    {
        return "self";
    }
    if ( Value.ParamIndex >= 0 )
    {
        // A generic parameter is spelled as the *owner* declared it: `T` on
        // Array, `U` on `def map<U>`. Anything else would name a hole by its
        // index, which is true and useless.
        const auto Slot = static_cast<std::size_t>( Value.ParamIndex );
        if ( Owner.IsValid() and Owner.Value < Store.TypeCount() and Slot < Store.Type( Owner ).Params.Size() )
        {
            return std::string( Store.Text( Store.Type( Owner ).Params[Slot] ) );
        }
        return "T" + std::to_string( Value.ParamIndex );
    }
    if ( not Value.Base.IsValid() or Value.Base.Value >= Store.TypeCount() )
    {
        return "?";
    }

    std::string Out( Store.Text( Store.Type( Value.Base ).Name ) );
    if ( Value.Args.Size() == 0 )
    {
        return Out;
    }

    Out += '<';
    for ( std::size_t Index = 0; Index < Value.Args.Size(); ++Index )
    {
        Out += Index == 0 ? "" : ", ";
        Out += DescribeSig( Value.Args[Index], Owner, Depth + 1 );
    }
    Out += '>';
    return Out;
}

Volt::Repl::Evaluator::MemberFact Volt::Repl::Evaluator::State::FactOf ( const MiddleEnd::TypeSystem::Member &Entry,
                                                                         const MiddleEnd::TypeSystem::NominalId Owner,
                                                                         const MiddleEnd::TypeSystem::NominalId AskedAbout ) const
{
    using namespace MiddleEnd::TypeSystem;

    const TypeStore &Store = TheDriver.Layouts();

    MemberFact Fact;
    Fact.Name    = std::string( Store.Text( Entry.Name ) );
    Fact.bMethod = Entry.Kind == EMemberKind::Method;
    Fact.Result  = DescribeSig( Entry.Result, Owner );

    if ( Owner != AskedAbout and Owner.IsValid() and Owner.Value < Store.TypeCount() )
    {
        Fact.Owner = std::string( Store.Text( Store.Type( Owner ).Name ) );
    }

    if ( not Fact.bMethod )
    {
        Fact.Signature = Fact.Name + " : " + Fact.Result;
        return Fact;
    }

    Fact.Signature = Fact.Name + "(";
    for ( std::size_t Index = 0; Index < Entry.Params.Size(); ++Index )
    {
        Fact.Signature += Index == 0 ? " " : ", ";
        // A `&block` parameter binds through a trailing block rather than
        // through the argument list, and writing it as a positional argument
        // would be an invitation to pass one.
        const bool bBlock = Index < Entry.ParamIsBlock.Size() and Entry.ParamIsBlock[Index];
        Fact.Signature += bBlock ? "&" : "";
        Fact.Signature += DescribeSig( Entry.Params[Index], Owner );
    }
    Fact.Signature += Entry.Params.Size() == 0 ? ")" : " )";
    return Fact;
}

void Volt::Repl::Evaluator::State::CollectMembers ( const MiddleEnd::TypeSystem::NominalId Id,
                                                    const MiddleEnd::TypeSystem::NominalId AskedAbout,
                                                    std::vector<MemberFact> &Out,
                                                    std::unordered_set<std::string> &Seen,
                                                    const std::uint32_t Depth ) const
{
    using namespace MiddleEnd::TypeSystem;

    const TypeStore &Store = TheDriver.Layouts();
    if ( not Id.IsValid() or Id.Value >= Store.TypeCount() or Depth > 16 )
    {
        return;
    }

    const NominalType &Type = Store.Type( Id );
    for ( const Member &Entry : Type.Members )
    {
        std::string Name( Store.Text( Entry.Name ) );
        // Own body first and shadowing accepted: a member a subclass redeclares
        // is offered once, as the subclass's, which is the one that would run.
        if ( not Seen.insert( Name ).second )
        {
            continue;
        }
        Out.push_back( FactOf( Entry, Id, AskedAbout ) );
    }

    // Last-included-first, the way LookupMember searches: `include Greeter`
    // then `include FormalGreeter` puts FormalGreeter nearer.
    for ( std::size_t Slot = Type.Includes.Size(); Slot > 0; --Slot )
    {
        CollectMembers( Store.Sig( Type.Includes[Slot - 1] ).Base, AskedAbout, Out, Seen, Depth + 1 );
    }
    if ( Type.Super.IsValid() )
    {
        CollectMembers( Store.Sig( Type.Super ).Base, AskedAbout, Out, Seen, Depth + 1 );
    }
}

// --- The queries themselves -------------------------------------------------------

Volt::Repl::Evaluator::TypeAnswer Volt::Repl::Evaluator::TypeOf ( const std::string_view Expression )
{
    TypeAnswer Out;
    if ( not Impl->bStarted )
    {
        Out.Diagnostics = "repl: the session was never started\n";
        return Out;
    }

    const State::Scratch Unit = Impl->Analyze( Expression, /*bBind=*/true );
    Out.Diagnostics           = Unit.Diagnostics;
    if ( not Unit.bOk )
    {
        return Out;
    }

    const MiddleEnd::TypeSystem::SemaTypeId Type = Impl->TypeOfBinding( Unit );
    if ( not Type.IsValid() )
    {
        Out.Diagnostics += "repl: that is a statement, not an expression with a type\n";
        return Out;
    }

    // Emitted, and then thrown away.
    //
    // The type was already known a line above this — reading it needs no
    // backend at all — so this exists for the other half of the answer: that
    // the expression is one the emitter can actually compile. A `:type` that
    // said `Int32` about something codegen would refuse would be a lie told
    // one command early. The emission opens no generation and reaches no
    // dylib, which is what makes asking free.
    std::string Error;
    const bool bEmitted = Impl->WithScratchView( Unit,
                                                 [&] ( const Backend::UnitView &View )
                                                 {
                                                     const Backend::BackendInput Build = Impl->Input();
                                                     return Impl->Jit.ProbeUnit( Build, View, nullptr, Error );
                                                 } );
    if ( not bEmitted )
    {
        Out.Diagnostics += Error + "\n";
    }

    Out.bOk  = true;
    Out.Name = Impl->DescribeType( Type );
    return Out;
}

Volt::Repl::Evaluator::LayoutAnswer Volt::Repl::Evaluator::LayoutOf ( const std::string_view Name )
{
    using namespace MiddleEnd::TypeSystem;

    LayoutAnswer Out;
    if ( not Impl->bStarted )
    {
        Out.Message = "repl: the session was never started";
        return Out;
    }

    const TypeStore &Store = Impl->TheDriver.Layouts();

    // A type name first, then — failing that — an expression, whose type is
    // what gets described. `:layout Int32` and `:layout x` both work, and the
    // second is usually the one a user actually wants.
    std::optional<NominalId> Which = Store.LookupType( Trim( Name ) );
    if ( not Which )
    {
        const State::Scratch Unit = Impl->Analyze( Name, /*bBind=*/true );
        if ( not Unit.bOk )
        {
            Out.Message = "repl: '" + std::string( Trim( Name ) ) + "' is neither a type nor an expression that compiles";
            return Out;
        }

        const SemaTypeId Type = Impl->TypeOfBinding( Unit );
        if ( Type.IsValid() and Store.Universe().Has( Type ) )
        {
            Which = Store.Universe().Get( Type ).Base;
        }
        if ( not Which or not Which->IsValid() )
        {
            Out.Message = "repl: nothing with a layout came out of that";
            return Out;
        }
    }

    const NominalType &Type = Store.Type( *Which );
    Out.Type                = std::string( Store.Text( Type.Name ) );

    if ( not Type.Layout.IsValid() )
    {
        // A generic that was never instantiated has no layout, and neither does
        // a type nothing has needed the shape of yet. Both are ordinary, not
        // errors.
        Out.Message = "repl: '" + Out.Type + "' has no resolved layout in this session";
        return Out;
    }

    const Backend::LayoutEngine Engine( Store );
    const Backend::SizeAlign Whole = Engine.Of( Type.Layout );

    Out.bOk   = true;
    Out.Size  = Whole.Size;
    Out.Align = Whole.Alignment;

    const LayoutNode &Node = Store.Get( Type.Layout );
    Out.Kind               = std::string( LayoutName( KindOf( Node ) ) );

    if ( const auto *Fields = std::get_if<Aggregate>( &Node ); Fields != nullptr )
    {
        for ( std::size_t Index = 0; Index < Fields->Fields.Size(); ++Index )
        {
            const FieldLayout &Field    = Fields->Fields[Index];
            const Backend::SizeAlign Of = Engine.Of( Field.Type );

            FieldFact Fact;
            Fact.Name   = std::string( Store.Text( Field.Name ) );
            Fact.Type   = std::string( LayoutName( KindOf( Store.Get( Field.Type ) ) ) );
            Fact.Offset = Engine.FieldOffset( Type.Layout, Index );
            Fact.Size   = Of.Size;
            Fact.Align  = Of.Alignment;

            // A primitive says what it *is* — `i32`, `ptr` — rather than that
            // it is a primitive. The spelling comes from the stdlib's own
            // `@[Primitive("i32")]`, so no Volt type name is written here.
            if ( const auto *Scalar = std::get_if<Primitive>( &Store.Get( Field.Type ) ); Scalar != nullptr )
            {
                Fact.Type = std::string( Store.Text( Scalar->Spelling ) );
            }

            Out.Fields.push_back( std::move( Fact ) );
        }
    }
    else if ( const auto *Scalar = std::get_if<Primitive>( &Node ); Scalar != nullptr )
    {
        Out.Fields.push_back( FieldFact{ .Name   = "(self)",
                                         .Type   = std::string( Store.Text( Scalar->Spelling ) ),
                                         .Offset = 0,
                                         .Size   = Whole.Size,
                                         .Align  = Whole.Alignment } );
    }

    return Out;
}

Volt::Repl::Evaluator::TextAnswer Volt::Repl::Evaluator::LastIr () const
{
    TextAnswer Out;
    Out.Text = Impl->Jit.LastUnitIr();
    Out.bOk  = not Out.Text.empty();
    if ( not Out.bOk )
    {
        Out.Message = "repl: no line has been compiled into this session yet";
    }
    return Out;
}

Volt::Repl::Evaluator::TextAnswer Volt::Repl::Evaluator::IrOf ( const std::string_view Expression )
{
    TextAnswer Out;
    if ( not Impl->bStarted )
    {
        Out.Message = "repl: the session was never started";
        return Out;
    }

    const State::Scratch Unit = Impl->Analyze( Expression, /*bBind=*/true );
    if ( not Unit.bOk )
    {
        Out.Message = Unit.Diagnostics;
        return Out;
    }

    std::string Error;
    std::string Text;
    const bool bEmitted = Impl->WithScratchView( Unit,
                                                 [&] ( const Backend::UnitView &View )
                                                 {
                                                     const Backend::BackendInput Build = Impl->Input();
                                                     return Impl->Jit.ProbeUnit( Build, View, &Text, Error );
                                                 } );
    if ( not bEmitted )
    {
        Out.Message = Error;
        return Out;
    }

    Out.bOk  = true;
    Out.Text = std::move( Text );
    return Out;
}

Volt::Repl::Evaluator::TextAnswer Volt::Repl::Evaluator::AsmOf ( const std::string_view Name, const std::size_t MaxBytes )
{
    using namespace MiddleEnd::TypeSystem;

    TextAnswer Out;
    if ( not Impl->bStarted )
    {
        Out.Message = "repl: the session was never started";
        return Out;
    }

    const std::string_view Wanted = Trim( Name );

    // A linker symbol, verbatim, first. `:asm _V_init_7` is how a REPL line's
    // own code is reached, and no Volt name spells it.
    std::uintptr_t Address = Impl->Jit.LookupSymbol( Wanted );
    std::string Symbol( Wanted );

    if ( Address == 0 )
    {
        const State::Found Which = Impl->Resolve( Wanted );
        if ( not Which.bOk or Which.Entry == nullptr )
        {
            Out.Message = "repl: " + ( Which.Message.empty() ? "'" + Symbol + "' did not resolve to anything" : Which.Message );
            return Out;
        }

        const TypeStore &Store = Impl->TheDriver.Layouts();

        // An `@[External]` member's symbol is the C spelling, verbatim — the
        // whole point of that boundary is that the linker and a C compiler
        // agree on the name, so mangling one would look for something nothing
        // defines.
        Symbol  = Which.Entry->ExternSymbol.IsValid() ? std::string( Store.Text( Which.Entry->ExternSymbol ) )
                                                      : Backend::MangleFunction( Store, *Which.Entry, Which.Owner, {} );
        Address = Impl->Jit.LookupSymbol( Symbol );
    }

    if ( Address == 0 )
    {
        Out.Message = "repl: '" + Symbol + "' is not materialised in this session";
        return Out;
    }

    Out.Text = Impl->Jit.Disassemble( Address, MaxBytes );
    Out.bOk  = not Out.Text.empty();
    if ( not Out.bOk )
    {
        Out.Message = "repl: nothing decoded at '" + Symbol + "'";
    }
    return Out;
}

Volt::Repl::Evaluator::TextAnswer Volt::Repl::Evaluator::SourceOf ( const std::string_view Name )
{
    TextAnswer Out;

    const State::Found Which = Impl->Resolve( Name );
    if ( not Which.bOk )
    {
        Out.Message = "repl: " + Which.Message;
        return Out;
    }

    const MiddleEnd::TypeSystem::TypeStore &Store = Impl->TheDriver.Layouts();
    const std::uint32_t Unit                      = Which.bType ? Store.Type( Which.Owner ).Unit : Which.Entry->Unit;
    const Frontend::DeclId Decl                   = Which.bType ? Store.Type( Which.Owner ).Decl : Which.Entry->Decl;

    const Core::SourceRange Range = Impl->RangeOf( Unit, Decl );
    const std::string_view Text   = Impl->TheDriver.SourceText( Range );
    if ( Text.empty() )
    {
        Out.Message = "repl: no source for '" + std::string( Trim( Name ) ) + "': " + Impl->WhyNoSource( Unit );
        return Out;
    }

    Out.bOk  = true;
    Out.Text = std::string( Text );
    return Out;
}

Volt::Repl::Evaluator::TextAnswer Volt::Repl::Evaluator::DocOf ( const std::string_view Name )
{
    TextAnswer Out;

    const State::Found Which = Impl->Resolve( Name );
    if ( not Which.bOk )
    {
        Out.Message = "repl: " + Which.Message;
        return Out;
    }

    const MiddleEnd::TypeSystem::TypeStore &Store = Impl->TheDriver.Layouts();
    const std::uint32_t Unit                      = Which.bType ? Store.Type( Which.Owner ).Unit : Which.Entry->Unit;
    const Frontend::DeclId Decl                   = Which.bType ? Store.Type( Which.Owner ).Decl : Which.Entry->Decl;

    const Core::SourceRange Range = Impl->RangeOf( Unit, Decl );
    const std::string_view File   = Impl->TheDriver.SourceFile( Range.File );
    if ( File.empty() or Range.Begin > File.size() )
    {
        Out.Message = "repl: no source for '" + std::string( Trim( Name ) ) + "': " + Impl->WhyNoSource( Unit );
        return Out;
    }

    // Doc comments never reach the token stream — the lexer skips them whole
    // (`SkipCommentOrDoc`, and there is no Doc row in TokenKind.inl) — so the
    // only place one survives is the text itself. Walking backwards from the
    // declaration is what finds it.
    //
    // Two shapes count, because the language has two and the stdlib uses both:
    // a `#{ ... #}` block, and a run of `#` lines. Blank lines and annotations
    // sit between a comment and what it documents without ending it; anything
    // else does, so a block belonging to whatever was declared above cannot be
    // mistaken for this one's.
    struct Row
    {

        std::size_t Begin = 0;
        std::size_t End   = 0;
    };

    // The line above `From`, or nothing when `From` is already at the top.
    const auto LineAbove = [&File] ( const std::size_t From ) -> std::optional<Row>
    {
        std::size_t End = From;
        while ( End > 0 and ( File[End - 1] == '\n' or File[End - 1] == '\r' ) )
        {
            --End;
        }
        if ( End == 0 )
        {
            return std::nullopt;
        }

        std::size_t Begin = End;
        while ( Begin > 0 and File[Begin - 1] != '\n' )
        {
            --Begin;
        }
        return Row{ .Begin = Begin, .End = End };
    };

    std::size_t Cursor = Range.Begin;
    while ( const std::optional<Row> Above = LineAbove( Cursor ) )
    {
        const std::string_view Text = Trim( File.substr( Above->Begin, Above->End - Above->Begin ) );

        if ( Text.empty() or Text.starts_with( "@[" ) )
        {
            Cursor = Above->Begin;
            continue;
        }

        if ( Text.ends_with( "#}" ) )
        {
            const std::size_t Close = File.rfind( "#}", Above->End );
            const std::size_t Open  = Close == std::string_view::npos ? std::string_view::npos : File.rfind( "#{", Close );
            if ( Open != std::string_view::npos )
            {
                Out.bOk  = true;
                Out.Text = std::string( File.substr( Open, Close + 2 - Open ) );
                return Out;
            }
            break;
        }

        if ( Text.starts_with( '#' ) )
        {
            // A run of line comments, taken as far up as it goes. The last one
            // found is the top of the block, so the walk keeps the earliest
            // start it saw.
            std::size_t Top      = Above->Begin;
            std::size_t Bottom   = Above->End;
            std::size_t Scanning = Above->Begin;
            while ( const std::optional<Row> More = LineAbove( Scanning ) )
            {
                if ( not Trim( File.substr( More->Begin, More->End - More->Begin ) ).starts_with( '#' ) )
                {
                    break;
                }
                Top      = More->Begin;
                Scanning = More->Begin;
            }

            Out.bOk  = true;
            Out.Text = std::string( File.substr( Top, Bottom - Top ) );
            return Out;
        }
        break;
    }

    Out.Message = "repl: nothing is documented above '" + std::string( Trim( Name ) ) + "'";
    return Out;
}

Volt::Repl::Evaluator::BenchAnswer Volt::Repl::Evaluator::Bench ( const std::string_view Expression,
                                                                  const std::size_t Iterations )
{
    BenchAnswer Out;
    if ( not Impl->bStarted )
    {
        Out.Message = "repl: the session was never started";
        return Out;
    }

    // Not bound: the value is thrown away every round, and binding it would
    // make the benchmark measure a store nobody asked for.
    const State::Scratch Unit = Impl->Analyze( Expression, /*bBind=*/false );
    Out.Diagnostics           = Unit.Diagnostics;
    if ( not Unit.bOk )
    {
        Out.Message = "repl: that does not compile";
        return Out;
    }

    const Backend::IJitBackend::BenchResult Ran =
        Impl->WithScratchView( Unit,
                               [&] ( const Backend::UnitView &View )
                               {
                                   const Backend::BackendInput Build = Impl->Input();
                                   return Impl->Jit.BenchUnit( Build, View, Iterations );
                               } );

    Out.bOk        = Ran.bOk;
    Out.Message    = Ran.Message;
    Out.Iterations = Ran.Iterations;
    Out.TotalNanos = Ran.TotalNanos;
    Out.BestNanos  = Ran.BestNanos;
    return Out;
}

// --- Reading the session, with nothing compiled -----------------------------------

std::vector<Volt::Repl::Evaluator::VariableFact> Volt::Repl::Evaluator::Variables () const
{
    std::vector<VariableFact> Out;
    Out.reserve( Impl->Vars.size() );
    for ( const State::SessionVar &Var : Impl->Vars )
    {
        // The internal bindings a bare expression is rewritten into are not
        // variables anybody typed, and offering them would be offering the
        // REPL's own bookkeeping back to its user.
        if ( Var.Name.starts_with( "__volt_repl_" ) )
        {
            continue;
        }
        Out.push_back( VariableFact{ .Name = Var.Name, .Type = Impl->DescribeType( Var.Type ) } );
    }
    return Out;
}

std::vector<Volt::Repl::Evaluator::MemberFact> Volt::Repl::Evaluator::MembersOf ( const std::string_view Expression )
{
    std::vector<MemberFact> Out;
    if ( not Impl->bStarted )
    {
        return Out;
    }

    // Typed, never emitted: a completion popup must not compile machine code,
    // and it certainly must not run any.
    const State::Scratch Unit = Impl->Analyze( Expression, /*bBind=*/true );
    if ( not Unit.bOk )
    {
        return Out;
    }

    const MiddleEnd::TypeSystem::SemaTypeId Type  = Impl->TypeOfBinding( Unit );
    const MiddleEnd::TypeSystem::TypeStore &Store = Impl->TheDriver.Layouts();
    if ( not Type.IsValid() or not Store.Universe().Has( Type ) )
    {
        return Out;
    }

    std::unordered_set<std::string> Seen;
    const MiddleEnd::TypeSystem::NominalId Base = Store.Universe().Get( Type ).Base;
    Impl->CollectMembers( Base, Base, Out, Seen );
    return Out;
}

std::vector<Volt::Repl::Evaluator::MemberFact> Volt::Repl::Evaluator::MembersOfType ( const std::string_view TypeName ) const
{
    std::vector<MemberFact> Out;

    const std::optional<MiddleEnd::TypeSystem::NominalId> Which = Impl->TheDriver.Layouts().LookupType( Trim( TypeName ) );
    if ( not Which )
    {
        return Out;
    }

    std::unordered_set<std::string> Seen;
    Impl->CollectMembers( *Which, *Which, Out, Seen );
    return Out;
}

std::vector<Volt::Repl::Evaluator::MemberFact> Volt::Repl::Evaluator::MembersOfModule ( const std::string_view ModuleName ) const
{
    const MiddleEnd::TypeSystem::TypeStore &Store = Impl->TheDriver.Layouts();
    std::vector<MemberFact> Out;

    const std::string TrimmedMod = std::string( Trim( ModuleName ) );
    for ( const MiddleEnd::TypeSystem::Member &Entry : Store.FreeFunctions() )
    {
        if ( Entry.Module.IsValid() and Store.Text( Entry.Module ) == TrimmedMod )
        {
            const std::string Name( Store.Text( Entry.Name ) );
            if ( not Name.starts_with( "__volt_" ) )
            {
                Out.push_back( Impl->FactOf( Entry, MiddleEnd::TypeSystem::NominalId{}, MiddleEnd::TypeSystem::NominalId{} ) );
            }
        }
    }
    return Out;
}

std::vector<std::string> Volt::Repl::Evaluator::FunctionNames () const
{
    const MiddleEnd::TypeSystem::TypeStore &Store = Impl->TheDriver.Layouts();

    std::vector<std::string> Out;
    for ( const MiddleEnd::TypeSystem::Member &Entry : Store.FreeFunctions() )
    {
        // If Entry is scoped inside a module (e.g. LibC.malloc, A.g), do NOT include it as a bare function name!
        if ( Entry.Module.IsValid() and not Store.Text( Entry.Module ).empty() )
        {
            continue;
        }

        std::string Name( Store.Text( Entry.Name ) );
        if ( not Name.starts_with( "__volt_" ) )
        {
            Out.push_back( std::move( Name ) );
        }
    }
    std::sort( Out.begin(), Out.end() );
    Out.erase( std::unique( Out.begin(), Out.end() ), Out.end() );
    return Out;
}

std::vector<std::string> Volt::Repl::Evaluator::TypeNames () const
{
    const MiddleEnd::TypeSystem::TypeStore &Store = Impl->TheDriver.Layouts();

    std::vector<std::string> Out;
    Out.reserve( Store.TypeCount() + Store.ModuleSymbols().size() );
    for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
    {
        Out.emplace_back(
            Store.Text( Store.Type( MiddleEnd::TypeSystem::NominalId{ static_cast<std::uint32_t>( Index ) } ).Name ) );
    }
    for ( const MiddleEnd::TypeSystem::Symbol Mod : Store.ModuleSymbols() )
    {
        std::string ModName( Store.Text( Mod ) );
        if ( not ModName.empty() and not ModName.starts_with( "__volt_" ) )
        {
            Out.push_back( std::move( ModName ) );
        }
    }
    std::sort( Out.begin(), Out.end() );
    Out.erase( std::unique( Out.begin(), Out.end() ), Out.end() );
    return Out;
}

bool Volt::Repl::Evaluator::KnowsType ( const std::string_view Name ) const
{
    return Impl->TheDriver.Layouts().LookupType( Name ).has_value();
}

bool Volt::Repl::Evaluator::KnowsModule ( const std::string_view Name ) const
{
    return Impl->TheDriver.Layouts().IsModule( Trim( Name ) );
}

bool Volt::Repl::Evaluator::KnowsFunction ( const std::string_view Name ) const
{
    return Impl->TheDriver.Layouts().LookupFunction( Name ) != nullptr;
}

std::size_t Volt::Repl::Evaluator::LiveGenerations () const
{
    return Impl->Jit.LiveGenerations();
}

bool Volt::Repl::Evaluator::Reset ( std::string &OutError )
{
    // Everything goes, in one move: the Driver, the JIT, the units, the type
    // store and the table of variables. A Driver cannot be emptied — it holds
    // a deque of non-movable units whose ASTs cache a pointer into themselves —
    // so the only honest reset is a new one.
    //
    // The old generations' code stays mapped until the JitBackend's destructor
    // runs, which is now. What a `:reset` costs is a fresh stdlib
    // materialisation, and that is what the artifact cache exists for.
    const EvaluatorOptions Options = Impl->Options;

    Impl = std::make_unique<State>();
    return Start( Options, OutError );
}
