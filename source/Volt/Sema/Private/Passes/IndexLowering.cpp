// IndexLowering.cpp — order 25. Turns subscripting into ordinary calls:
//
//     obj[ a, b ]       ->  Call( Member( obj, "[]"  ), [ a, b ] )
//     obj[ a ] = value  ->  Call( Member( obj, "[]=" ), [ a, value ] )
//
// `obj[ a ] += value` needs no case of its own: AssignLowering (order 24) has
// already turned it into `obj[ a ] = obj[ a ] + value`, i.e. one store and one
// read, both handled below.
//
// "[]" and "[]=" are operator *spellings*, exactly like TokenSpelling( Op )
// for a Binary node — never a Volt type name, so the zero-hardcode guard is
// satisfied. IndexOperator already existed for the type checker's own lookup
// and is shared from here.
//
// Lowering is also what gives an index its type: before it, the Index branch
// of ExprInferencer called MemberType directly and returned an invalid
// SemaTypeId on a generic receiver, so `take( arr[0] )` checked nothing. Now
// the type comes from CallType, hence from CheckCallArgs, like any call.

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Sema/Pass.hpp"

#include <cstddef>
#include <string_view>
#include <variant>

namespace
{

using namespace Volt;

// The two subscript operators, spelled as the language spells them.
constexpr std::string_view IndexRead  = "[]";
constexpr std::string_view IndexWrite = "[]=";

class IndexRewriter
{

public:

    explicit IndexRewriter ( Frontend::AstContext &InContext ) : Context( InContext )
    {
    }

    std::size_t Run ()
    {
        // Index sweep, copy-out / write-back — see rules/ast-rewrite.md.
        //
        // Stores first: recognising `obj[ a ] = v` needs its target to still
        // be an Index node. The read sweep then rewrites everything left,
        // including the targets just consumed — those are unreferenced by
        // then, and rewriting them is what keeps the arena free of any
        // residual sugar node for AstInvariant to find.
        const std::size_t OriginalCount = Context.ExprCount();
        std::size_t Rewritten           = 0;

        for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            if ( KindOf( Context.Expr( Id ) ) != Frontend::ExprKind::Assign )
            {
                continue;
            }

            const Frontend::Assign Node = std::get<Frontend::Assign>( Context.Expr( Id ) );
            if ( not Node.Target.IsValid() or KindOf( Context.Expr( Node.Target ) ) != Frontend::ExprKind::Index )
            {
                continue;
            }

            Context.Expr( Id ) = LowerStore( Node );
            ++Rewritten;
        }

        for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            if ( KindOf( Context.Expr( Id ) ) == Frontend::ExprKind::Index )
            {
                Context.Expr( Id ) = LowerRead( Id );
                ++Rewritten;
            }
        }

        return Rewritten;
    }

private:

    [[nodiscard]] Frontend::ExprId MakeCallee ( Frontend::ExprId Object, Core::SourceRange Loc, std::string_view Operator )
    {
        Frontend::Member Mem;
        Mem.Loc    = Loc;
        Mem.Object = Object;
        Mem.Name   = Context.Strings().Intern( Operator );
        return Context.Add( Frontend::ExprNode{ Mem } );
    }

    [[nodiscard]] Frontend::ExprNode LowerRead ( Frontend::ExprId IndexId )
    {
        const Frontend::Index Node = std::get<Frontend::Index>( Context.Expr( IndexId ) );

        Frontend::Call Call;
        Call.Loc    = Node.Loc;
        Call.Callee = MakeCallee( Node.Object, Node.Loc, IndexRead );
        Call.Args   = Node.Args;
        for ( std::size_t Arg = 0; Arg < Node.Args.Size(); ++Arg )
        {
            Call.ArgNames.PushBack( Core::Symbol{} );
        }
        return Call;
    }

    [[nodiscard]] Frontend::ExprNode LowerStore ( const Frontend::Assign &Node )
    {
        const Frontend::Index Target = std::get<Frontend::Index>( Context.Expr( Node.Target ) );

        Frontend::Call Call;
        Call.Loc    = Node.Loc;
        Call.Callee = MakeCallee( Target.Object, Target.Loc, IndexWrite );
        Call.Args   = Target.Args;
        Call.Args.PushBack( Node.Value );
        for ( std::size_t Arg = 0; Arg < Call.Args.Size(); ++Arg )
        {
            Call.ArgNames.PushBack( Core::Symbol{} );
        }
        return Call;
    }

    Frontend::AstContext &Context;
};

} // namespace

void Volt::MiddleEnd::Lowering::IndexLowering ( Core::PassContext &Context )
{
    Context.Stats.IndexesLowered += IndexRewriter( Context.Ast ).Run();
}
