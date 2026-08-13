// InterpLowering.cpp — order 26. Rewrites `"a#{ x }b"` into the concatenation
// it means:
//
//   Binary( +, Binary( +, "a", Call( Member( x, to_string ) ) ), "b" )
//
// Folded to the left, in strict source order, so the result is what a reader
// would write by hand and the backends never see an Interp node.
//
// Order 26 — after MacroExpansion (15): a macro body is text, and that text
// routinely contains `#{ ... }` which only becomes an Interp node once the
// macro has been expanded and re-parsed. Lowering earlier would miss exactly
// the interpolations a macro generates.
//
// `to_string` is a *method name*, not a type name, so the zero-hardcode guard
// is satisfied — the same precedent JsxLowering sets with `create_element`.
// The alternative, an `@[Stringify]` annotation, would force this pass to know
// each part's type in order to find the annotated member, which means running
// after TypeChecker, which means hand-annotating the types of every node it
// creates. The fixed name is the less indebted choice.

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"

#include <cstddef>
#include <string_view>
#include <variant>

namespace
{

using namespace Volt;

constexpr std::string_view StringifyMethod = "to_string";

class InterpRewriter
{

public:

    explicit InterpRewriter ( Frontend::AstContext &InContext ) : Context( InContext )
    {
    }

    std::size_t Run ()
    {
        // Index sweep, copy-out / write-back — see rules/ast-rewrite.md.
        const std::size_t OriginalCount = Context.ExprCount();
        std::size_t Rewritten           = 0;

        for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            if ( KindOf( Context.Expr( Id ) ) == Frontend::ExprKind::Interp )
            {
                Context.Expr( Id ) = LowerInterp( Id );
                ++Rewritten;
            }
        }

        return Rewritten;
    }

private:

    /// A part that is already a string chunk is concatenated as it stands;
    /// anything else is asked for its string form.
    [[nodiscard]] Frontend::ExprId Stringify ( Frontend::ExprId Part, Volt::Core::SourceRange Loc )
    {
        if ( KindOf( Context.Expr( Part ) ) == Frontend::ExprKind::StringLiteral )
        {
            return Part;
        }

        Frontend::Member Mem;
        Mem.Loc                       = Loc;
        Mem.Object                    = Part;
        Mem.Name                      = Context.Strings().Intern( StringifyMethod );
        const Frontend::ExprId Callee = Context.Add( Frontend::ExprNode{ Mem } );

        Frontend::Call Node;
        Node.Loc    = Loc;
        Node.Callee = Callee;
        return Context.Add( Frontend::ExprNode{ Node } );
    }

    [[nodiscard]] Frontend::ExprNode LowerInterp ( Frontend::ExprId InterpId )
    {
        const Frontend::Interp Node = std::get<Frontend::Interp>( Context.Expr( InterpId ) );

        // `""` written with no parts at all: the empty string is still a
        // StringLiteral, so the slot stays a core node.
        if ( Node.Parts.Size() == 0 )
        {
            Frontend::StringLiteral Empty;
            Empty.Loc   = Node.Loc;
            Empty.Value = Context.Strings().Intern( "" );
            return Frontend::ExprNode{ Empty };
        }

        Frontend::ExprId Accumulator = Stringify( Node.Parts[0], Node.Loc );

        for ( std::size_t Index = 1; Index < Node.Parts.Size(); ++Index )
        {
            const Frontend::ExprId Rhs = Stringify( Node.Parts[Index], Node.Loc );

            Frontend::Binary Concat;
            Concat.Loc  = Node.Loc;
            Concat.Op   = Frontend::TokenKind::Plus;
            Concat.Lhs  = Accumulator;
            Concat.Rhs  = Rhs;
            Accumulator = Context.Add( Frontend::ExprNode{ Concat } );
        }

        // A single part folds to no Binary at all: the slot becomes the
        // stringified part itself, whose type is already the string type.
        return Context.Expr( Accumulator );
    }

    Frontend::AstContext &Context;
};

} // namespace

void Volt::MiddleEnd::Lowering::InterpLowering ( Core::PassContext &Context )
{
    Context.Stats.InterpsLowered += InterpRewriter( Context.Ast ).Run();
}
