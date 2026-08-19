// AssignLowering.cpp — order 24. Normalises compound assignment:
//
//     target op= value   ->   target = Binary( op, target', value )
//
// where `target'` is a fresh copy of the target node, so the read side and
// the write side are two distinct AST nodes with two distinct roles.
//
// Until now `Assign::Op` was carried all the way to the backends and read by
// nobody: the TypeChecker's Assign branch ignores it, so `s += 1` on a type
// with no `+` resolved to nothing at all. Desugaring here routes every
// compound form through the same operator resolution as `a + b`, and leaves
// exactly one shape of Assign — a store — in the core AST.
//
// The operator set is not hardcoded: `IsAssignment` comes from the
// VOLT_ASSIGN rows of Pratt.inl, and the base operator is derived from the
// spelling (`+=` minus its `=` is `+`). A new compound operator is one line
// in the manifest and nothing here.

#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/Parser/Pratt.hpp"
#include "Volt/MiddleEnd/Core/Pass.hpp"
#include "Volt/MiddleEnd/Resolver/ScopeTable.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

namespace
{

using namespace Volt;

// The infix operator behind a compound assignment, found by spelling rather
// than by a hand-kept table: `+=` is the punctuation spelled `+` plus `=`.
// Returns Error when Op is not a compound assignment.
[[nodiscard]] Frontend::TokenKind BaseOperatorOf ( Frontend::TokenKind Op )
{
    if ( Op == Frontend::TokenKind::Assign or not Frontend::IsAssignment( Op ) )
    {
        return Frontend::TokenKind::Error;
    }

    const std::string_view Spelling = Frontend::TokenSpelling( Op );
    if ( Spelling.size() < 2 or Spelling.back() != '=' )
    {
        return Frontend::TokenKind::Error;
    }

    const std::string_view Base = Spelling.substr( 0, Spelling.size() - 1 );
    for ( std::size_t Kind = 0; Kind < static_cast<std::size_t>( Frontend::TokenKind::Count ); ++Kind )
    {
        const auto Candidate = static_cast<Frontend::TokenKind>( Kind );
        if ( not Frontend::IsAssignment( Candidate ) and Frontend::TokenSpelling( Candidate ) == Base )
        {
            return Candidate;
        }
    }
    return Frontend::TokenKind::Error;
}

// A node that may be read twice without changing the meaning of the program.
// Deliberately shallow: a Call or an operator could run user code, and the
// compiler has no purity information to lean on.
[[nodiscard]] bool IsSideEffectFree ( const Frontend::ExprNode &Node )
{
    switch ( KindOf( Node ) )
    {
    case Frontend::ExprKind::Identifier:
    case Frontend::ExprKind::InstanceVar:
    case Frontend::ExprKind::SelfExpr:
    case Frontend::ExprKind::IntLiteral:
    case Frontend::ExprKind::FloatLiteral:
    case Frontend::ExprKind::StringLiteral:
    case Frontend::ExprKind::CharLiteral:
    case Frontend::ExprKind::BoolLiteral:
    case Frontend::ExprKind::SymbolLiteral:
    case Frontend::ExprKind::NilLiteral:
        return true;
    default:
        return false;
    }
}

class AssignRewriter
{

public:

    explicit AssignRewriter ( MiddleEnd::Core::PassContext &InContext ) : Context( InContext ), Ast( InContext.Ast )
    {
    }

    std::size_t Run ()
    {
        // Index sweep, copy-out / write-back — see rules/ast-rewrite.md.
        const std::size_t OriginalCount = Ast.ExprCount();
        std::size_t Rewritten           = 0;

        for ( std::size_t Index = 0; Index < OriginalCount; ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            const Frontend::ExprKind Kind = KindOf( Ast.Expr( Id ) );

            if ( Kind == Frontend::ExprKind::Assign )
            {
                const Frontend::Assign AssignNode = std::get<Frontend::Assign>( Ast.Expr( Id ) );
                if ( BaseOperatorOf( AssignNode.Op ) != Frontend::TokenKind::Error )
                {
                    if ( const std::optional<Frontend::ExprNode> Lowered = LowerCompound( AssignNode ) )
                    {
                        Ast.Expr( Id ) = *Lowered;
                        ++Rewritten;
                    }
                }
            }
            else if ( Kind == Frontend::ExprKind::Unary )
            {
                const Frontend::Unary UnaryNode = std::get<Frontend::Unary>( Ast.Expr( Id ) );
                if ( UnaryNode.Op == Frontend::TokenKind::PlusPlus or UnaryNode.Op == Frontend::TokenKind::MinusMinus )
                {
                    if ( const std::optional<Frontend::ExprNode> Lowered = LowerPreIncDec( UnaryNode ) )
                    {
                        Ast.Expr( Id ) = *Lowered;
                        ++Rewritten;
                    }
                }
            }
            else if ( Kind == Frontend::ExprKind::Postfix )
            {
                const Frontend::Postfix PostNode = std::get<Frontend::Postfix>( Ast.Expr( Id ) );
                if ( PostNode.Op == Frontend::TokenKind::PlusPlus or PostNode.Op == Frontend::TokenKind::MinusMinus )
                {
                    if ( const std::optional<Frontend::ExprNode> Lowered = LowerPostIncDec( PostNode, Id ) )
                    {
                        Ast.Expr( Id ) = *Lowered;
                        ++Rewritten;
                    }
                }
            }
        }

        return Rewritten;
    }

private:

    // A node this pass mints is a *second occurrence of the same use*, so it
    // inherits the target's binding rather than being left unresolved:
    // ScopeResolver ran at order 10 and will not run again, and every consumer
    // — TypeChecker's local types, codegen's slot table — reaches a local
    // exclusively through BindingOf. Counting it is right too: `x += 1` reads
    // x. Nothing to inherit (a Member/Index target, or an unbound name) simply
    // leaves the clone as it was.
    [[nodiscard]] Frontend::ExprId CloneUse ( Frontend::ExprId TargetId, Frontend::ExprNode Node )
    {
        const Frontend::ExprId Fresh = Ast.Add( std::move( Node ) );
        if ( const MiddleEnd::Resolver::Binding *Bound = Context.Scopes.BindingOf( TargetId ) )
        {
            Context.Scopes.BindUse( Fresh, *Bound );
        }
        return Fresh;
    }

    // A fresh node playing the *read* role, sharing the target's children.
    // Sharing is what makes `arr[i] += 1` legal without a temporary: the
    // children are re-read, never re-evaluated for effect, which is why they
    // must all be side-effect free.
    [[nodiscard]] std::optional<Frontend::ExprId> CloneForRead ( Frontend::ExprId TargetId )
    {
        const Frontend::ExprNode Target = Ast.Expr( TargetId );

        if ( IsSideEffectFree( Target ) )
        {
            return CloneUse( TargetId, Frontend::ExprNode{ Target } );
        }

        if ( const auto *Mem = std::get_if<Frontend::Member>( &Target ) )
        {
            const Frontend::Member MemCopy = *Mem;
            if ( not IsSideEffectFree( Ast.Expr( MemCopy.Object ) ) )
            {
                return std::nullopt;
            }
            return CloneUse( TargetId, Frontend::ExprNode{ MemCopy } );
        }

        if ( const auto *Idx = std::get_if<Frontend::Index>( &Target ) )
        {
            const Frontend::Index IdxCopy = *Idx;
            if ( not IsSideEffectFree( Ast.Expr( IdxCopy.Object ) ) )
            {
                return std::nullopt;
            }
            for ( const Frontend::ExprId Arg : IdxCopy.Args )
            {
                if ( not IsSideEffectFree( Ast.Expr( Arg ) ) )
                {
                    return std::nullopt;
                }
            }
            return CloneUse( TargetId, Frontend::ExprNode{ IdxCopy } );
        }

        return std::nullopt;
    }

    [[nodiscard]] std::optional<Frontend::ExprNode> LowerCompound ( const Frontend::Assign &Node )
    {
        const std::optional<Frontend::ExprId> Read = CloneForRead( Node.Target );
        if ( not Read.has_value() )
        {
            Context.Diags.Report(
                Volt::Core::Diagnostic{ .Severity = Volt::Core::ESeverity::Error,
                                        .Range    = Node.Loc,
                                        .Message  = "'" + std::string{ Frontend::TokenSpelling( Node.Op ) } +
                                                   "' needs a target that can be read twice — assign through a local instead",
                                        .Notes = {} } );
            return std::nullopt;
        }

        Frontend::Binary Combined;
        Combined.Loc = Node.Loc;
        Combined.Op  = BaseOperatorOf( Node.Op );
        Combined.Lhs = *Read;
        Combined.Rhs = Node.Value;

        Frontend::Assign Store;
        Store.Loc    = Node.Loc;
        Store.Op     = Frontend::TokenKind::Assign;
        Store.Target = Node.Target;
        Store.Value  = Ast.Add( Frontend::ExprNode{ Combined } );
        return Frontend::ExprNode{ Store };
    }

    [[nodiscard]] std::optional<Frontend::ExprNode> LowerPreIncDec ( const Frontend::Unary &Node )
    {
        const std::optional<Frontend::ExprId> Read = CloneForRead( Node.Operand );
        if ( not Read.has_value() )
        {
            Context.Diags.Report(
                Volt::Core::Diagnostic{ .Severity = Volt::Core::ESeverity::Error,
                                        .Range    = Node.Loc,
                                        .Message  = "'" + std::string{ Frontend::TokenSpelling( Node.Op ) } +
                                                   "' needs a target that can be read twice — assign through a local instead",
                                        .Notes = {} } );
            return std::nullopt;
        }

        Frontend::Unary Inc;
        Inc.Loc     = Node.Loc;
        Inc.Op      = Node.Op;
        Inc.Operand = *Read;

        Frontend::Assign Store;
        Store.Loc    = Node.Loc;
        Store.Op     = Frontend::TokenKind::Assign;
        Store.Target = Node.Operand;
        Store.Value  = Ast.Add( Frontend::ExprNode{ Inc } );
        return Frontend::ExprNode{ Store };
    }

    [[nodiscard]] std::optional<Frontend::ExprNode> LowerPostIncDec ( const Frontend::Postfix &Node, Frontend::ExprId NodeId )
    {
        const std::optional<Frontend::ExprId> ReadInitial = CloneForRead( Node.Operand );
        const std::optional<Frontend::ExprId> ReadForInc  = CloneForRead( Node.Operand );
        if ( not ReadInitial.has_value() or not ReadForInc.has_value() )
        {
            Context.Diags.Report(
                Volt::Core::Diagnostic{ .Severity = Volt::Core::ESeverity::Error,
                                        .Range    = Node.Loc,
                                        .Message  = "'" + std::string{ Frontend::TokenSpelling( Node.Op ) } +
                                                   "' needs a target that can be read twice — assign through a local instead",
                                        .Notes = {} } );
            return std::nullopt;
        }

        MiddleEnd::Resolver::ScopeId CurrentScope = Context.Scopes.ScopeOfExpr( NodeId );
        if ( not CurrentScope.IsValid() )
        {
            CurrentScope = Context.Scopes.ScopeOfExpr( Node.Operand );
        }
        if ( not CurrentScope.IsValid() )
        {
            CurrentScope = MiddleEnd::Resolver::ScopeId{ 0 };
        }

        const Frontend::Symbol TmpName = Ast.MakeUniqueSymbol( "__post_tmp" );
        const Frontend::ExprId TmpTarget =
            Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Node.Loc, .Name = TmpName } } );

        Context.Scopes.Declare( CurrentScope, TmpName, MiddleEnd::Resolver::BindingSite{ TmpTarget } );
        const MiddleEnd::Resolver::Binding *Bound = Context.Scopes.Resolve( CurrentScope, TmpName );
        if ( Bound != nullptr )
        {
            Context.Scopes.BindUse( TmpTarget, *Bound, false );
        }
        Context.Scopes.SetScopeOfExpr( TmpTarget, CurrentScope );

        // 1. __post_tmp = x
        Frontend::Assign SaveAssign;
        SaveAssign.Loc                = Node.Loc;
        SaveAssign.Op                 = Frontend::TokenKind::Assign;
        SaveAssign.Target             = TmpTarget;
        SaveAssign.Value              = *ReadInitial;
        const Frontend::ExprId SaveId = Ast.Add( Frontend::ExprNode{ SaveAssign } );
        const Frontend::StmtId Stmt1  = Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Node.Loc, .Expr = SaveId } } );

        // 2. x = Unary{ Op, x'' }
        Frontend::Unary Inc;
        Inc.Loc     = Node.Loc;
        Inc.Op      = Node.Op;
        Inc.Operand = *ReadForInc;

        Frontend::Assign Store;
        Store.Loc                      = Node.Loc;
        Store.Op                       = Frontend::TokenKind::Assign;
        Store.Target                   = Node.Operand;
        Store.Value                    = Ast.Add( Frontend::ExprNode{ Inc } );
        const Frontend::ExprId StoreId = Ast.Add( Frontend::ExprNode{ Store } );
        const Frontend::StmtId Stmt2   = Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Node.Loc, .Expr = StoreId } } );

        // 3. __post_tmp
        const Frontend::ExprId TmpUse = Ast.Add( Frontend::ExprNode{ Frontend::Identifier{ .Loc = Node.Loc, .Name = TmpName } } );
        if ( Bound != nullptr )
        {
            Context.Scopes.BindUse( TmpUse, *Bound, true );
        }
        Context.Scopes.SetScopeOfExpr( TmpUse, CurrentScope );
        const Frontend::StmtId Stmt3 = Ast.Add( Frontend::StmtNode{ Frontend::ExprStmt{ .Loc = Node.Loc, .Expr = TmpUse } } );

        Frontend::StmtList Body;
        Body.PushBack( Stmt1 );
        Body.PushBack( Stmt2 );
        Body.PushBack( Stmt3 );

        Frontend::BeginExpr BeginNode;
        BeginNode.Loc           = Node.Loc;
        BeginNode.Body          = std::move( Body );
        BeginNode.RescueClauses = {};
        BeginNode.EnsureBody    = {};

        return Frontend::ExprNode{ BeginNode };
    }

    MiddleEnd::Core::PassContext &Context;
    Frontend::AstContext &Ast;
};

} // namespace

void Volt::MiddleEnd::Lowering::AssignLowering ( Core::PassContext &Context )
{
    Context.Stats.AssignsLowered += AssignRewriter( Context ).Run();
}
