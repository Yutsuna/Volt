// MacroExpansion.cpp — order 15. The half of compile-time evaluation that
// needs no type information: host command literals (`` `git rev-parse HEAD` ``)
// and the manifest operations over the values they produce
// (`` `uname`.trim ``, `` `find ...`.lines ``).
//
// What is *not* here is `macro def`. A macro-generated method has to be a
// member of its target type, and the TypeStore is frozen by the time any pass
// runs — a method added here would never be found by name and never emitted
// (DeclareSweep.cpp iterates the store, not the ASTs). That expansion is a
// serial seam step instead: ConstEval::ExpandTypeMacros, next to
// SynthesizeFinalizeStubs, where the store is still mutable.
//
// The sweep runs over the Expr arena by ascending index, which is what makes a
// chain fold in one pass: a sub-expression always has a smaller index than the
// node above it, so `` `cmd`.trim `` sees a StringLiteral receiver by the time
// it is reached. Nodes are copied out and written back, never held across an
// Add() (rules/ast-rewrite.md).

#include "MacroValue.hpp"

#include "Volt/Core/Diagnostics/SourceManager.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using namespace Volt;
using namespace Volt::Frontend;
using namespace Volt::MiddleEnd;
using namespace Volt::MiddleEnd::Core;
using namespace Volt::MiddleEnd::ConstEval;

class CommandFolder
{

public:

    explicit CommandFolder ( PassContext &InContext ) : Context( InContext ), Ast( InContext.Ast )
    {
    }

    std::size_t Run ()
    {
        ResolveWorkDir();
        MarkCallees();
        // A macro body is a compile-time program the interface seam has
        // already run (ConstEval::ExpandTypeMacros). Its nodes are still in
        // the arena — arenas only grow — so a sweep by index reaches them, and
        // folding one again would run its commands a *second* time, with the
        // side effects and the duplicate diagnostics that implies. The mask is
        // the same one the type checker and the AstInvariant census consult.
        Metadata = Frontend::MetadataExprs( Ast );

        const std::size_t Count = Ast.ExprCount();
        for ( std::size_t Index = 0; Index < Count; ++Index )
        {
            Fold( ExprId{ static_cast<ExprId::ValueType>( Index ) } );
        }
        return Folded;
    }

private:

    // A command runs where its file lives, so a relative path in it means what
    // the source says — the same directory `__DIR__` reports.
    void ResolveWorkDir ()
    {
        if ( Context.Sources == nullptr or not Context.Sources->IsValidFile( Ast.FileId() ) )
        {
            return;
        }
        const std::string_view Path = Context.Sources->PathOf( Ast.FileId() );
        const std::size_t Slash     = Path.find_last_of( '/' );
        WorkDir                     = Slash == std::string_view::npos ? std::string{} : std::string( Path.substr( 0, Slash ) );
    }

    // `text.chomp( "," )` reaches the arena as a Call whose callee is the
    // Member — and the Member has the smaller index, so the sweep would fold it
    // first, as a no-argument `chomp`. Marking callees is what keeps the
    // arguments attached to their operation.
    void MarkCallees ()
    {
        bIsCallee.assign( Ast.ExprCount(), false );
        for ( std::size_t Index = 0; Index < Ast.ExprCount(); ++Index )
        {
            const auto *Node = std::get_if<Call>( &Ast.Expr( ExprId{ static_cast<ExprId::ValueType>( Index ) } ) );
            if ( Node != nullptr and Node->Callee.IsValid() and Node->Callee.Value < bIsCallee.size() )
            {
                bIsCallee[Node->Callee.Value] = true;
            }
        }
    }

    void Fold ( ExprId Id )
    {
        if ( Id.Value < Metadata.size() and Metadata[Id.Value] )
        {
            return;
        }
        std::visit( Meta::Overloaded{ [&] ( const CommandLit &Node ) { FoldCommand( Id, Node ); },
                                      [&] ( const Member &Node )
                                      {
                                          if ( not bIsCallee[Id.Value] )
                                          {
                                              FoldOperation( Id, Node, {} );
                                          }
                                      },
                                      [&] ( const Call &Node ) { FoldCall( Id, Node ); }, [] ( const auto & ) {} },
                    Ast.Expr( Id ) );
    }

    void FoldCall ( ExprId Id, const Call &Node )
    {
        if ( not Node.Callee.IsValid() or Node.BlockArg.IsValid() )
        {
            return;
        }
        const auto *Callee = std::get_if<Member>( &Ast.Expr( Node.Callee ) );
        if ( Callee == nullptr or not IsMacroOp( Ast.Text( Callee->Name ) ) )
        {
            return;
        }

        std::vector<MacroValue> Args;
        Args.reserve( Node.Args.Size() );
        for ( const ExprId Arg : Node.Args )
        {
            std::optional<MacroValue> Value = ValueOfLiteral( Ast, Arg );
            if ( not Value )
            {
                return; // a runtime argument: an ordinary call, not a fold
            }
            Args.push_back( std::move( *Value ) );
        }
        FoldOperation( Id, *Callee, Args );
    }

    void FoldOperation ( ExprId Id, const Member &Node, std::span<const MacroValue> Args )
    {
        const std::string_view Spelling = Ast.Text( Node.Name );
        if ( not IsMacroOp( Spelling ) )
        {
            return;
        }
        const std::optional<MacroValue> Receiver = ValueOfLiteral( Ast, Node.Object );
        if ( not Receiver )
        {
            return; // a runtime receiver keeps its runtime method call
        }

        const ::Volt::Core::SourceRange Loc      = Node.Loc;
        const std::optional<MacroValue> Produced = ApplyMacroOp( Spelling, *Receiver, Args, Context.Diags, Loc );
        if ( not Produced )
        {
            return;
        }
        Replace( Id, *Produced, Loc );
    }

    void FoldCommand ( ExprId Id, const CommandLit &Node )
    {
        const ::Volt::Core::SourceRange Loc = Node.Loc;

        std::string Command;
        for ( const ExprId Part : Node.Parts )
        {
            if ( const auto *Chunk = std::get_if<StringLiteral>( &Ast.Expr( Part ) ) )
            {
                // Verbatim: the escapes in a command are the shell's, and
                // decoding them here would rewrite the command being run.
                Command += Ast.Text( Chunk->Value );
                continue;
            }
            const std::optional<MacroValue> Value = ValueOfLiteral( Ast, Part );
            if ( not Value )
            {
                Context.Diags.Error( Loc, "a command's interpolation must be known at compile time" );
                Replace( Id, MacroValue{ std::string{} }, Loc );
                return;
            }
            Command += Stringify( *Value );
        }

        Replace( Id, RunMacroCommand( Command, WorkDir, Context.Diags, Loc ), Loc );
    }

    // Built before the slot is assigned: a sequence value adds its elements to
    // the very arena the slot lives in, and the reference would not survive it.
    void Replace ( ExprId Id, const MacroValue &Value, ::Volt::Core::SourceRange Loc )
    {
        ExprNode Literal = LiteralOfValue( Ast, Value, Loc );
        Ast.Expr( Id )   = std::move( Literal );
        ++Folded;
    }

    PassContext &Context;
    AstContext &Ast;
    std::string WorkDir;
    std::vector<bool> bIsCallee;
    std::vector<bool> Metadata;
    std::size_t Folded = 0;
};

} // namespace

namespace Volt::MiddleEnd::ConstEval
{

void MacroExpansion ( PassContext &Context )
{
    Context.Stats.MacrosExpanded += CommandFolder( Context ).Run();
}

} // namespace Volt::MiddleEnd::ConstEval
