#include "MacroEval.hpp"

#include "Volt/Core/Log/Logger.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstClone.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/AST/Type.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using namespace Volt;
using namespace Volt::Frontend;
using namespace Volt::MiddleEnd::ConstEval;

namespace TypeSystem = Volt::MiddleEnd::TypeSystem;

// --- The call shapes the evaluator carries out, from MacroCalls.inl ---------

enum class EMacroCall : std::uint8_t
{

#define VOLT_MACRO_CALL( Name, Spelling ) Name,
#include "Volt/MiddleEnd/ConstEval/MacroCalls.inl"
};

constexpr std::array MacroCallSpellings = {
#define VOLT_MACRO_CALL( Name, Spelling ) std::string_view( Spelling ),
#include "Volt/MiddleEnd/ConstEval/MacroCalls.inl"
};

[[nodiscard]] constexpr std::string_view SpellingOf ( EMacroCall Which )
{
    return MacroCallSpellings[static_cast<std::size_t>( Which )];
}

[[nodiscard]] std::optional<EMacroCall> MacroCallOf ( std::string_view Spelling )
{
    for ( std::size_t Index = 0; Index < MacroCallSpellings.size(); ++Index )
    {
        if ( MacroCallSpellings[Index] == Spelling )
        {
            return static_cast<EMacroCall>( Index );
        }
    }
    return std::nullopt;
}

// --- Bounds (R8) -----------------------------------------------------------
//
// Neighbours of MaxFinalizeDepth, and there for the same reason: a malformed
// program must stop with a diagnostic, never hang the build.
constexpr std::uint32_t MaxMacroDepth   = 32;
constexpr std::uint32_t MaxBodyCommands = 256;

// --- Compile-time operators ------------------------------------------------
//
// Deliberately *not* part of MacroOps.inl: an operator is not one of the
// operations a value may be folded away by (R2), it is only one the evaluator
// can answer when it has to decide a branch. That distinction is the whole of
// why `if `uname`.trim == "Linux"` picks a branch at compile time while
// `assert!( content.size > 0 )` survives into the generated method with its
// operands folded and its comparison intact.
[[nodiscard]] std::optional<MacroValue> ApplyBinary ( TokenKind Op, const MacroValue &Lhs, const MacroValue &Rhs )
{
    switch ( Op )
    {
    case TokenKind::KwAnd:
    case TokenKind::AndAnd:
        return MacroValue{ Truthy( Lhs ) and Truthy( Rhs ) };
    case TokenKind::KwOr:
    case TokenKind::OrOr:
        return MacroValue{ Truthy( Lhs ) or Truthy( Rhs ) };
    default:
        break;
    }

    const auto *LeftText  = std::get_if<std::string>( &Lhs.Data );
    const auto *RightText = std::get_if<std::string>( &Rhs.Data );
    if ( LeftText != nullptr and RightText != nullptr )
    {
        switch ( Op )
        {
        case TokenKind::Plus:
            return MacroValue{ *LeftText + *RightText };
        case TokenKind::EqEq:
        case TokenKind::TripleEq:
            return MacroValue{ *LeftText == *RightText };
        case TokenKind::NotEq:
            return MacroValue{ *LeftText != *RightText };
        case TokenKind::Lt:
            return MacroValue{ *LeftText < *RightText };
        case TokenKind::Le:
            return MacroValue{ *LeftText <= *RightText };
        case TokenKind::Gt:
            return MacroValue{ *LeftText > *RightText };
        case TokenKind::Ge:
            return MacroValue{ *LeftText >= *RightText };
        default:
            return std::nullopt;
        }
    }

    const auto *LeftInt  = std::get_if<std::int64_t>( &Lhs.Data );
    const auto *RightInt = std::get_if<std::int64_t>( &Rhs.Data );
    if ( LeftInt != nullptr and RightInt != nullptr )
    {
        switch ( Op )
        {
        case TokenKind::Plus:
            return MacroValue{ *LeftInt + *RightInt };
        case TokenKind::Minus:
            return MacroValue{ *LeftInt - *RightInt };
        case TokenKind::Star:
            return MacroValue{ *LeftInt * *RightInt };
        // A division by zero stays runtime rather than becoming a
        // compile-time answer nobody can give: the emitted code keeps the
        // operation, and whatever the program does about it, it does.
        case TokenKind::Slash:
            return *RightInt == 0 ? std::nullopt : std::optional{ MacroValue{ *LeftInt / *RightInt } };
        case TokenKind::Percent:
            return *RightInt == 0 ? std::nullopt : std::optional{ MacroValue{ *LeftInt % *RightInt } };
        case TokenKind::EqEq:
        case TokenKind::TripleEq:
            return MacroValue{ *LeftInt == *RightInt };
        case TokenKind::NotEq:
            return MacroValue{ *LeftInt != *RightInt };
        case TokenKind::Lt:
            return MacroValue{ *LeftInt < *RightInt };
        case TokenKind::Le:
            return MacroValue{ *LeftInt <= *RightInt };
        case TokenKind::Gt:
            return MacroValue{ *LeftInt > *RightInt };
        case TokenKind::Ge:
            return MacroValue{ *LeftInt >= *RightInt };
        default:
            return std::nullopt;
        }
    }

    const auto *LeftBool  = std::get_if<bool>( &Lhs.Data );
    const auto *RightBool = std::get_if<bool>( &Rhs.Data );
    if ( LeftBool != nullptr and RightBool != nullptr )
    {
        switch ( Op )
        {
        case TokenKind::EqEq:
        case TokenKind::TripleEq:
            return MacroValue{ *LeftBool == *RightBool };
        case TokenKind::NotEq:
            return MacroValue{ *LeftBool != *RightBool };
        default:
            return std::nullopt;
        }
    }

    return std::nullopt;
}

/// The plain operator behind a compound assignment, or Error when the token is
/// not one — `n += 1` against a compile-time binding is `n = n + 1`.
[[nodiscard]] constexpr TokenKind CompoundBase ( TokenKind Op )
{
    switch ( Op )
    {
    case TokenKind::PlusEq:
        return TokenKind::Plus;
    case TokenKind::MinusEq:
        return TokenKind::Minus;
    case TokenKind::StarEq:
        return TokenKind::Star;
    case TokenKind::SlashEq:
        return TokenKind::Slash;
    case TokenKind::PercentEq:
        return TokenKind::Percent;
    default:
        return TokenKind::Error;
    }
}

/// A written type, rendered back into the words the source used. Never a
/// resolved type: this runs before signatures are, and a generated method is
/// written in the user's own vocabulary anyway (rules/zero-hardcode.md).
[[nodiscard]] std::string TypeSpelling ( const AstContext &Ast, TypeId Id )
{
    if ( not Id.IsValid() )
    {
        return {};
    }

    return std::visit( Meta::Overloaded{ [&] ( const TypeRef &Node )
                                         {
                                             std::string Out;
                                             for ( std::size_t Index = 0; Index < Node.Path.Size(); ++Index )
                                             {
                                                 Out += Index > 0 ? "::" : "";
                                                 Out += Ast.Text( Node.Path[Index] );
                                             }
                                             for ( std::size_t Index = 0; Index < Node.Generics.Size(); ++Index )
                                             {
                                                 Out += Index > 0 ? ", " : "<";
                                                 Out += TypeSpelling( Ast, Node.Generics[Index] );
                                             }
                                             return Node.Generics.IsEmpty() ? Out : Out + ">";
                                         },
                                         [&] ( const PointerType &Node ) { return TypeSpelling( Ast, Node.Pointee ) + "*"; },
                                         [&] ( const NilableType &Node ) { return TypeSpelling( Ast, Node.Inner ) + "?"; },
                                         [&] ( const FixedArrayType &Node ) { return TypeSpelling( Ast, Node.Elem ) + "[]"; },
                                         [&] ( const DynamicType &Node ) { return "&" + TypeSpelling( Ast, Node.Trait ); },
                                         [&] ( const FuncType &Node )
                                         {
                                             std::string Out = "(";
                                             for ( std::size_t Index = 0; Index < Node.Params.Size(); ++Index )
                                             {
                                                 Out += Index > 0 ? ", " : "";
                                                 Out += TypeSpelling( Ast, Node.Params[Index] );
                                             }
                                             return Out + ") -> " + TypeSpelling( Ast, Node.Return );
                                         },
                                         [] ( const auto & ) { return std::string{}; } },
                       Ast.Type( Id ) );
}

/// Any of the node lists, whatever its inline capacity — one constraint
/// instead of the three same_as arms a fixed SmallVec size would need.
template <typename ListType, typename ElementType>
concept ListOf = requires( ListType &List ) {
    { List[0] } -> std::same_as<ElementType &>;
    { List.Size() } -> std::convertible_to<std::size_t>;
};

// One evaluation of one macro body. Everything the staging rules need is
// state of this object: the bindings a compile-time source has produced, and
// the per-statement memo of what each source node was worth.
class Evaluator
{

public:

    explicit Evaluator ( MacroEnv &InEnv ) : Env( InEnv ), Src( InEnv.Source ), Dst( InEnv.Target )
    {
    }

    void Run ( const StmtList &Body, StmtList &Out, bool bTailValue )
    {
        if ( Env.Depth > MaxMacroDepth )
        {
            Env.Diags.Error( ::Volt::Core::SourceRange{},
                             "macro expansion nested deeper than " + std::to_string( MaxMacroDepth ) + " levels" );
            return;
        }
        EvalStmts( Body, Out, bTailValue );
    }

private:

    /// What one source expression is worth to the evaluator.
    ///
    /// The two flags are not the same question. `bSource` answers R2 — does
    /// this flow from compile-time *data* — and decides whether a binding
    /// disappears or stays a runtime local. `bFold` answers R6 — is this one of
    /// the operations the evaluator may *substitute a value for* — and decides
    /// what the generated code looks like. An operator has a value and no fold:
    /// computable, so a branch over it is decided here; not substitutable, so
    /// the comparison itself survives into the emitted call.
    struct EvalResult
    {

        std::optional<MacroValue> Value;
        bool bSource = false;
        bool bFold   = false;
    };

    using MemoMap = std::unordered_map<std::uint32_t, EvalResult>;

    /// A statement's own fold memo. Each turn of an unrolled loop re-evaluates
    /// the same source nodes against different bindings, so the memo cannot
    /// outlive the statement it was built for — and cannot be dropped either,
    /// or a command would run once per mention instead of once.
    class MemoFrame
    {

    public:

        explicit MemoFrame ( Evaluator &InOwner ) : Owner( InOwner ), Saved( std::move( InOwner.Memo ) )
        {
            Owner.Memo.clear();
        }

        MemoFrame ( const MemoFrame & )           = delete;
        MemoFrame ( MemoFrame && )                = delete;
        MemoFrame &operator=( const MemoFrame & ) = delete;
        MemoFrame &operator=( MemoFrame && )      = delete;

        ~MemoFrame ()
        {
            Owner.Memo = std::move( Saved );
        }

    private:

        Evaluator &Owner;
        MemoMap Saved;
    };

    // --- Statements --------------------------------------------------------

    void EvalStmts ( const StmtList &Body, StmtList &Out, bool bTailValue )
    {
        for ( std::size_t Index = 0; Index < Body.Size(); ++Index )
        {
            EvalStmt( Body[Index], Out, bTailValue and Index + 1 == Body.Size() );
        }
    }

    [[nodiscard]] StmtList EvalStmtList ( const StmtList &Body, bool bTail = false )
    {
        StmtList Out;
        EvalStmts( Body, Out, bTail );
        return Out;
    }

    void EvalStmt ( StmtId Id, StmtList &Out, bool bTail )
    {
        if ( not Id.IsValid() )
        {
            return;
        }

        const MemoFrame Frame( *this );
        // Copied out of the arena before anything is added to it: Source and
        // Target may be the same AstContext (rules/ast-rewrite.md).
        const StmtNode Node = Src.Stmt( Id );

        std::visit( Meta::Overloaded{ [&] ( const ExprStmt &Stmt ) { EvalExprStmt( Stmt, Out, bTail ); },
                                      [&] ( const LocalDecl &Stmt ) { EvalLocalDecl( Stmt, Out ); },
                                      [&] ( const auto &Stmt ) { EmitStmtNode( Stmt, Out ); } },
                    Node );
    }

    // An expression statement is where every compile-time *construct* lives:
    // the parser leaves `if`, `case` and `for` in expression position, so this
    // is the one place that has both the shape and somewhere to emit to.
    void EvalExprStmt ( const ExprStmt &Stmt, StmtList &Out, bool bTail )
    {
        if ( not Stmt.Expr.IsValid() )
        {
            return;
        }

        const ExprNode Node = Src.Expr( Stmt.Expr );

        if ( const auto *Branch = std::get_if<If>( &Node ) )
        {
            EvalIf( *Branch, Out, bTail );
            return;
        }
        if ( const auto *Selection = std::get_if<CaseExpr>( &Node ); Selection != nullptr and EvalCase( *Selection, Out, bTail ) )
        {
            return;
        }
        if ( const auto *Loop = std::get_if<Call>( &Node ); Loop != nullptr and EvalEachLoop( *Loop, Out ) )
        {
            return;
        }
        if ( const auto *Store = std::get_if<Assign>( &Node ); Store != nullptr and EvalAssign( *Store, Out ) )
        {
            return;
        }

        const EvalResult Result = Eval( Stmt.Expr );
        if ( not Result.Value )
        {
            PushExpr( Out, Emit( Stmt.Expr ), Stmt.Loc );
            return;
        }

        // Executed, so nothing is emitted — that is what makes `puts` and a
        // bare command literal compile-time actions. In tail position the
        // value *is* the generated method's result, so it materialises; a nil
        // one (`puts` is the only producer) has nothing to return.
        if ( bTail and not std::holds_alternative<std::monostate>( Result.Value->Data ) )
        {
            PushExpr( Out, Dst.Add( LiteralOfValue( Dst, *Result.Value, Stmt.Loc ) ), Stmt.Loc );
        }
    }

    void EvalLocalDecl ( const LocalDecl &Stmt, StmtList &Out )
    {
        const EvalResult Result = Eval( Stmt.Init );
        if ( Result.Value and Result.bSource )
        {
            Bindings[std::string( Src.Text( Stmt.Name ) )] = *Result.Value;
            return;
        }

        LocalDecl Copy;
        Copy.Loc      = Stmt.Loc;
        Copy.Name     = Dst.Strings().Intern( Src.Text( Stmt.Name ) );
        Copy.DeclType = CloneType( Src, Dst, Stmt.DeclType );
        Copy.Init     = Emit( Stmt.Init );
        Out.PushBack( Dst.Add( StmtNode{ std::move( Copy ) } ) );
    }

    // `if` over a condition the evaluator can answer is not emitted at all:
    // only the winning branch is visited, which is what "conditional
    // compilation without a directive" means. A runtime condition keeps its
    // `if` — an ordinary branch inside a generated method is ordinary code —
    // and both of its arms are still evaluated, so a loop nested in one still
    // unrolls.
    void EvalIf ( const If &Node, StmtList &Out, bool bTail )
    {
        const EvalResult Cond = Eval( Node.Cond );
        if ( Cond.Value )
        {
            EvalStmts( Truthy( *Cond.Value ) ? Node.Then : Node.Else, Out, bTail );
            return;
        }

        If Copy;
        Copy.Loc  = Node.Loc;
        Copy.Cond = Emit( Node.Cond );
        Copy.Then = EvalStmtList( Node.Then, bTail );
        Copy.Else = EvalStmtList( Node.Else, bTail );
        PushExpr( Out, Dst.Add( ExprNode{ std::move( Copy ) } ), Node.Loc );
    }

    /// `case` over a compile-time scrutinee, decided here. Returns false when
    /// anything about it is runtime, leaving the node to ordinary emission.
    [[nodiscard]] bool EvalCase ( const CaseExpr &Node, StmtList &Out, bool bTail )
    {
        const EvalResult Target = Eval( Node.Target );
        if ( not Target.Value )
        {
            return false;
        }

        for ( const StmtId ClauseId : Node.Clauses )
        {
            if ( not ClauseId.IsValid() )
            {
                continue;
            }
            const auto *Clause = std::get_if<WhenClause>( &Src.Stmt( ClauseId ) );
            if ( Clause == nullptr )
            {
                return false;
            }

            const WhenClause Copy = *Clause;
            for ( const ExprId Pattern : Copy.Patterns )
            {
                const EvalResult Value = Eval( Pattern );
                if ( not Value.Value )
                {
                    return false; // one runtime pattern and the whole case is runtime
                }
                const std::optional<MacroValue> Match = ApplyBinary( TokenKind::EqEq, *Target.Value, *Value.Value );
                if ( Match and Truthy( *Match ) )
                {
                    EvalStmts( Copy.Body, Out, bTail );
                    return true;
                }
            }
        }

        EvalStmts( Node.ElseBody, Out, bTail );
        return true;
    }

    /// `for x in seq` — which reaches here as `seq.each { |x| ... }`, the only
    /// shape it can have (the parser desugars every `for` that way, and there
    /// is no `For` node). A compile-time sequence unrolls; anything else is an
    /// ordinary runtime loop and is emitted as written.
    [[nodiscard]] bool EvalEachLoop ( const Call &Node, StmtList &Out )
    {
        if ( not Node.Callee.IsValid() or not Node.BlockArg.IsValid() )
        {
            return false;
        }
        const auto *Callee = std::get_if<Member>( &Src.Expr( Node.Callee ) );
        if ( Callee == nullptr or Src.Text( Callee->Name ) != SpellingOf( EMacroCall::Each ) )
        {
            return false;
        }
        const Member Receiver = *Callee;

        const auto *BlockNode = std::get_if<Block>( &Src.Expr( Node.BlockArg ) );
        if ( BlockNode == nullptr )
        {
            return false;
        }
        const Block Body = *BlockNode;

        const EvalResult Sequence = Eval( Receiver.Object );
        if ( not Sequence.Value )
        {
            return false;
        }
        const auto *Elements = std::get_if<std::vector<MacroValue>>( &Sequence.Value->Data );
        if ( Elements == nullptr )
        {
            Env.Diags.Error( Node.Loc, "a compile-time loop needs a sequence to iterate" );
            return true;
        }

        // The loop variables shadow whatever they shadow for the length of the
        // unrolling, and give it back afterwards — a `for` does not leak its
        // variable into the body that follows it.
        std::vector<std::string> Names;
        std::vector<std::optional<MacroValue>> Shadowed;
        for ( const ParamId Param : Body.Params )
        {
            Names.emplace_back( Src.Text( Src.GetParam( Param ).Name ) );
            const auto Existing = Bindings.find( Names.back() );
            Shadowed.push_back( Existing == Bindings.end() ? std::nullopt : std::optional{ Existing->second } );
        }

        for ( const MacroValue &Element : *Elements )
        {
            BindLoopVariables( Names, Element );
            EvalStmts( Body.Body, Out, false );
        }

        for ( std::size_t Index = 0; Index < Names.size(); ++Index )
        {
            if ( Shadowed[Index] )
            {
                Bindings[Names[Index]] = *Shadowed[Index];
            }
            else
            {
                Bindings.erase( Names[Index] );
            }
        }
        return true;
    }

    // `for key, value in pairs` destructures an element that is itself a
    // sequence; one variable takes the element whole.
    void BindLoopVariables ( const std::vector<std::string> &Names, const MacroValue &Element )
    {
        const auto *Parts = std::get_if<std::vector<MacroValue>>( &Element.Data );
        if ( Names.size() == 1 or Parts == nullptr )
        {
            for ( const std::string &Name : Names )
            {
                Bindings[Name] = Element;
            }
            return;
        }
        for ( std::size_t Index = 0; Index < Names.size(); ++Index )
        {
            Bindings[Names[Index]] = Index < Parts->size() ? ( *Parts )[Index] : MacroValue{};
        }
    }

    /// `name = value` / `name += value`. Returns true when the assignment was
    /// a compile-time binding and nothing is emitted for it.
    [[nodiscard]] bool EvalAssign ( const Assign &Node, StmtList &Out )
    {
        static_cast<void>( Out );
        const auto *Target = std::get_if<Identifier>( &Src.Expr( Node.Target ) );
        if ( Target == nullptr )
        {
            return false; // an ivar or an index: runtime storage, emitted
        }

        const std::string Name( Src.Text( Target->Name ) );
        const EvalResult Result = Eval( Node.Value );
        const auto Bound        = Bindings.find( Name );

        if ( Node.Op == TokenKind::Assign )
        {
            if ( Result.Value and Result.bSource )
            {
                Bindings[Name] = *Result.Value;
                return true;
            }
            if ( Bound != Bindings.end() )
            {
                // Earlier mentions were already folded into the emitted code;
                // letting this one through would leave the generated method
                // assigning to a local nothing ever declared.
                Env.Diags.Error( Node.Loc, "'" + Name + "' is a compile-time binding and cannot be given a runtime value" );
                return true;
            }
            return false;
        }

        if ( Bound == Bindings.end() )
        {
            return false; // an ordinary compound assignment on a runtime local
        }
        const std::optional<MacroValue> Combined =
            Result.Value ? ApplyBinary( CompoundBase( Node.Op ), Bound->second, *Result.Value ) : std::nullopt;
        if ( not Combined )
        {
            Env.Diags.Error( Node.Loc, "'" + Name + "' is a compile-time binding and cannot be updated by this assignment" );
            return true;
        }
        Bound->second = *Combined;
        return true;
    }

    // --- Evaluation --------------------------------------------------------

    [[nodiscard]] EvalResult Eval ( ExprId Id )
    {
        if ( not Id.IsValid() )
        {
            return {};
        }
        if ( const auto Known = Memo.find( Id.Value ); Known != Memo.end() )
        {
            return Known->second;
        }

        EvalResult Result = Compute( Id );
        Memo.emplace( Id.Value, Result );
        return Result;
    }

    [[nodiscard]] EvalResult Compute ( ExprId Id )
    {
        const ExprNode Node = Src.Expr( Id );

        return std::visit(
            Meta::Overloaded{ [&] ( const Identifier &Expr ) { return EvalIdentifier( Expr ); }, [&] ( const SelfExpr & )
                              { return EvalSelf(); }, [&] ( const Member &Expr ) { return EvalMember( Expr ); },
                              [&] ( const Call &Expr ) { return EvalCall( Expr ); }, [&] ( const Binary &Expr )
                              { return EvalBinary( Expr ); }, [&] ( const Unary &Expr ) { return EvalUnary( Expr ); },
                              [&] ( const Interp &Expr ) { return EvalParts( Expr.Parts, false ); },
                              [&] ( const CommandLit &Expr ) { return EvalCommand( Expr ); },
                              [&] ( const ArrayLit &Expr ) { return EvalArray( Expr ); },
                              // `@#{ ... }` names an instance variable: a runtime value
                              // whose *name* is compile-time, which is a matter for
                              // emission, not for evaluation.
                              [&] ( const IvarInterp & ) { return EvalResult{}; },
                              [&] ( const auto & )
                              {
                                  // A literal is a compile-time value but never a
                                  // compile-time *source* (R2): `json = "{"` stays a runtime
                                  // local, which is what makes the generated method build
                                  // its string at run time instead of at compile time.
                                  std::optional<MacroValue> Literal = ValueOfLiteralNode( Src, Node );
                                  const bool bKnown                 = Literal.has_value();
                                  return EvalResult{ .Value = std::move( Literal ), .bSource = false, .bFold = bKnown };
                              } },
            Node );
    }

    [[nodiscard]] EvalResult EvalIdentifier ( const Identifier &Node )
    {
        const std::string_view Spelling = Src.Text( Node.Name );
        if ( const auto Bound = Bindings.find( std::string( Spelling ) ); Bound != Bindings.end() )
        {
            return EvalResult{ .Value = Bound->second, .bSource = true, .bFold = true };
        }

        // `__DIR__` & co come from the one manifest MagicExpansion reads: one
        // vocabulary, two consumers, no second list to keep in step.
        if ( IsMagicShape( Spelling ) )
        {
            if ( std::optional<ExprNode> Expanded = ExpandMagic( Spelling, Env.Site, Dst.Strings(), Node.Loc ) )
            {
                std::optional<MacroValue> Value = ValueOfLiteralNode( Dst, *Expanded );
                if ( Value )
                {
                    return EvalResult{ .Value = std::move( Value ), .bSource = true, .bFold = true };
                }
            }
        }
        return {};
    }

    [[nodiscard]] EvalResult EvalSelf ()
    {
        if ( not Env.SelfType.IsValid() )
        {
            return {}; // a `macro do` has no target type; `self` is not a source
        }
        if ( not SelfDesc )
        {
            SelfDesc = DescribeSelf();
        }
        return EvalResult{ .Value = MacroValue{ *SelfDesc }, .bSource = true, .bFold = true };
    }

    // The target type as introspection sees it: its name, and its own fields
    // in declaration order, each with the *spelling* its declaration wrote.
    // Read off the store's member list (the same one every other consumer
    // reads) and the declaring unit's AST, which is where a written type lives
    // — Member::Result is not resolved yet at this seam, and does not need to
    // be.
    [[nodiscard]] MacroTypeDesc DescribeSelf () const
    {
        MacroTypeDesc Desc;
        const TypeSystem::NominalType &Type = Env.Store.Type( Env.SelfType );
        Desc.Name                           = std::string( Env.Store.Text( Type.Name ) );

        for ( const TypeSystem::Member &Entry : Type.Members )
        {
            if ( Entry.Kind != TypeSystem::EMemberKind::Field )
            {
                continue;
            }
            const AstContext *Owner = UnitAst( Entry.Unit );
            if ( Owner == nullptr or not Entry.Decl.IsValid() )
            {
                continue;
            }
            const auto *Field = std::get_if<Frontend::Field>( &Owner->Decl( Entry.Decl ) );
            if ( Field == nullptr )
            {
                continue;
            }
            Desc.Fields.push_back( MacroFieldDesc{ .Name = std::string( Owner->Text( Field->Name ) ),
                                                   .Type = TypeSpelling( *Owner, Field->DeclType ) } );
        }
        return Desc;
    }

    [[nodiscard]] const AstContext *UnitAst ( std::uint32_t Unit ) const
    {
        return Unit < Env.Units.size() ? Env.Units[Unit] : nullptr;
    }

    [[nodiscard]] EvalResult EvalMember ( const Member &Node )
    {
        const EvalResult Object = Eval( Node.Object );
        if ( not Object.Value )
        {
            return {};
        }
        const std::string_view Spelling = Src.Text( Node.Name );
        if ( not IsMacroOp( Spelling ) )
        {
            return {};
        }
        std::optional<MacroValue> Produced = ApplyMacroOp( Spelling, *Object.Value, {}, Env.Diags, Node.Loc );
        if ( not Produced )
        {
            return {};
        }
        return EvalResult{ .Value = std::move( Produced ), .bSource = Object.bSource, .bFold = true };
    }

    [[nodiscard]] EvalResult EvalCall ( const Call &Node )
    {
        if ( not Node.Callee.IsValid() )
        {
            return {};
        }
        const ExprNode Callee = Src.Expr( Node.Callee );

        std::vector<MacroValue> Args;
        bool bArgsKnown  = true;
        bool bArgsSource = false;
        for ( const ExprId Arg : Node.Args )
        {
            const EvalResult Value = Eval( Arg );
            if ( not Value.Value )
            {
                bArgsKnown = false;
                break;
            }
            Args.push_back( *Value.Value );
            bArgsSource = bArgsSource or Value.bSource;
        }

        if ( const auto *Name = std::get_if<Identifier>( &Callee ) )
        {
            const std::optional<EMacroCall> Which = MacroCallOf( Src.Text( Name->Name ) );
            if ( Which != EMacroCall::Puts or not bArgsKnown or Node.BlockArg.IsValid() )
            {
                return {}; // an ordinary call: emitted, its arguments folded
            }
            std::string Line;
            for ( const MacroValue &Arg : Args )
            {
                Line += Stringify( Arg );
            }
            ::Volt::Core::FLogger::Info( std::move( Line ), "macro" );
            return EvalResult{ .Value = MacroValue{}, .bSource = true, .bFold = true };
        }

        const auto *Receiver = std::get_if<Member>( &Callee );
        if ( Receiver == nullptr or Node.BlockArg.IsValid() or not bArgsKnown )
        {
            return {};
        }
        const std::string_view Spelling = Src.Text( Receiver->Name );
        if ( not IsMacroOp( Spelling ) )
        {
            return {};
        }
        // Note the receiver is evaluated through its own sub-expression and
        // never through the callee node: a `Member` in callee position is half
        // of a call, and evaluating it on its own would answer the argument-
        // less question (`json.chomp` instead of `json.chomp( "," )`).
        const EvalResult Object = Eval( Receiver->Object );
        if ( not Object.Value )
        {
            return {};
        }
        std::optional<MacroValue> Produced = ApplyMacroOp( Spelling, *Object.Value, Args, Env.Diags, Receiver->Loc );
        if ( not Produced )
        {
            return {};
        }
        return EvalResult{ .Value = std::move( Produced ), .bSource = Object.bSource or bArgsSource, .bFold = true };
    }

    [[nodiscard]] EvalResult EvalBinary ( const Binary &Node )
    {
        const EvalResult Lhs = Eval( Node.Lhs );
        const EvalResult Rhs = Eval( Node.Rhs );
        if ( not Lhs.Value or not Rhs.Value )
        {
            return {};
        }
        std::optional<MacroValue> Produced = ApplyBinary( Node.Op, *Lhs.Value, *Rhs.Value );
        if ( not Produced )
        {
            return {};
        }
        // Computable, not substitutable: see EvalResult and ApplyBinary.
        return EvalResult{ .Value = std::move( Produced ), .bSource = Lhs.bSource or Rhs.bSource, .bFold = false };
    }

    [[nodiscard]] EvalResult EvalUnary ( const Unary &Node )
    {
        const EvalResult Operand = Eval( Node.Operand );
        if ( not Operand.Value )
        {
            return {};
        }

        std::optional<MacroValue> Produced;
        if ( Node.Op == TokenKind::KwNot or Node.Op == TokenKind::Bang )
        {
            Produced = MacroValue{ not Truthy( *Operand.Value ) };
        }
        else if ( Node.Op == TokenKind::Minus )
        {
            if ( const auto *Number = std::get_if<std::int64_t>( &Operand.Value->Data ) )
            {
                Produced = MacroValue{ -*Number };
            }
        }
        if ( not Produced )
        {
            return {};
        }
        return EvalResult{ .Value = std::move( Produced ), .bSource = Operand.bSource, .bFold = false };
    }

    /// The segments of an interpolation or of a command: literal chunks and
    /// spliced expressions, in source order. bVerbatim keeps a chunk exactly
    /// as written, which is what a command needs — the escapes in it are the
    /// shell's, and decoding them here would rewrite the command being run.
    [[nodiscard]] EvalResult EvalParts ( const ExprList &Parts, bool bVerbatim )
    {
        std::string Text;
        bool bSource = false;
        for ( const ExprId Part : Parts )
        {
            if ( bVerbatim )
            {
                if ( const auto *Chunk = std::get_if<StringLiteral>( &Src.Expr( Part ) ) )
                {
                    Text += Src.Text( Chunk->Value );
                    continue;
                }
            }
            const EvalResult Value = Eval( Part );
            if ( not Value.Value )
            {
                return {};
            }
            Text += Stringify( *Value.Value );
            bSource = bSource or Value.bSource;
        }
        return EvalResult{ .Value = MacroValue{ std::move( Text ) }, .bSource = bSource, .bFold = true };
    }

    [[nodiscard]] EvalResult EvalCommand ( const CommandLit &Node )
    {
        const EvalResult Command = EvalParts( Node.Parts, true );
        if ( not Command.Value )
        {
            Env.Diags.Error( Node.Loc, "a command's interpolation must be known at compile time" );
            return EvalResult{ .Value = MacroValue{ std::string{} }, .bSource = true, .bFold = true };
        }

        if ( ++Commands > MaxBodyCommands )
        {
            Env.Diags.Error( Node.Loc, "a macro body may run at most " + std::to_string( MaxBodyCommands ) + " commands" );
            return EvalResult{ .Value = MacroValue{ std::string{} }, .bSource = true, .bFold = true };
        }

        MacroValue Output = RunMacroCommand( Stringify( *Command.Value ), Env.WorkDir, Env.Diags, Node.Loc );
        return EvalResult{ .Value = std::move( Output ), .bSource = true, .bFold = true };
    }

    [[nodiscard]] EvalResult EvalArray ( const ArrayLit &Node )
    {
        std::vector<MacroValue> Elements;
        bool bSource = false;
        for ( const ExprId Element : Node.Elements )
        {
            const EvalResult Value = Eval( Element );
            if ( not Value.Value )
            {
                return {};
            }
            Elements.push_back( *Value.Value );
            bSource = bSource or Value.bSource;
        }
        return EvalResult{ .Value = MacroValue{ std::move( Elements ) }, .bSource = bSource, .bFold = true };
    }

    // --- Emission ----------------------------------------------------------

    /// The source node as real code in the target type's arena: its foldable
    /// parts replaced by the values they were worth, everything else copied
    /// with its children mapped through here.
    [[nodiscard]] ExprId Emit ( ExprId Id )
    {
        if ( not Id.IsValid() )
        {
            return ExprId{};
        }

        const EvalResult Result             = Eval( Id );
        const ExprNode Node                 = Src.Expr( Id );
        const ::Volt::Core::SourceRange Loc = LocOf( Node );

        if ( Result.Value and Result.bFold )
        {
            return Dst.Add( LiteralOfValue( Dst, *Result.Value, Loc ) );
        }

        if ( const auto *Ivar = std::get_if<IvarInterp>( &Node ) )
        {
            return EmitIvarInterp( *Ivar );
        }
        if ( const auto *Invocation = std::get_if<Call>( &Node ) )
        {
            return EmitCall( *Invocation );
        }

        return std::visit(
            [&] ( const auto &Alternative ) -> ExprId
            {
                using AlternativeType = std::remove_cvref_t<decltype( Alternative )>;
                if constexpr ( Meta::Reflected<AlternativeType> )
                {
                    AlternativeType Copy = Alternative;
                    MapFields( Copy );
                    return Dst.Add( ExprNode{ std::move( Copy ) } );
                }
                else
                {
                    return ExprId{};
                }
            },
            Node );
    }

    // `@#{ field.name }` is the one node whose emission is a different kind of
    // node: an ordinary instance variable, whose interned lexeme keeps the `@`
    // (MemberResolver.cpp reads the name that way).
    [[nodiscard]] ExprId EmitIvarInterp ( const IvarInterp &Node )
    {
        const EvalResult Name = Eval( Node.Name );
        if ( not Name.Value )
        {
            Env.Diags.Error( Node.Loc, "'@#{ ... }' needs an instance variable name known at compile time" );
            return Dst.Add( ExprNode{ NilLiteral{ .Loc = Node.Loc } } );
        }
        return Dst.Add(
            ExprNode{ InstanceVar{ .Loc = Node.Loc, .Name = Dst.Strings().Intern( "@" + Stringify( *Name.Value ) ) } } );
    }

    // A call is emitted by hand for one reason: its callee must *not* go
    // through Emit. A `Member` in callee position is half of the call, and
    // folding it on its own would answer the argument-less question — turning
    // `json.chomp( "," )` into whatever `json.chomp` is worth.
    [[nodiscard]] ExprId EmitCall ( const Call &Node )
    {
        Call Copy   = Node;
        Copy.Callee = EmitCallee( Node.Callee );
        for ( std::size_t Index = 0; Index < Copy.Args.Size(); ++Index )
        {
            Copy.Args[Index] = Emit( Node.Args[Index] );
        }
        for ( std::size_t Index = 0; Index < Copy.ArgNames.Size(); ++Index )
        {
            Copy.ArgNames[Index] = Reintern( Node.ArgNames[Index] );
        }
        Copy.BlockArg = Emit( Node.BlockArg );
        return Dst.Add( ExprNode{ std::move( Copy ) } );
    }

    [[nodiscard]] ExprId EmitCallee ( ExprId Id )
    {
        if ( not Id.IsValid() )
        {
            return ExprId{};
        }
        const auto *Access = std::get_if<Member>( &Src.Expr( Id ) );
        if ( Access == nullptr )
        {
            return Emit( Id );
        }
        const Member Node = *Access;
        return Dst.Add( ExprNode{ Member{ .Loc = Node.Loc, .Object = Emit( Node.Object ), .Name = Reintern( Node.Name ) } } );
    }

    template <typename NodeType> void EmitStmtNode ( const NodeType &Node, StmtList &Out )
    {
        if constexpr ( Meta::Reflected<NodeType> )
        {
            NodeType Copy = Node;
            MapFields( Copy );
            Out.PushBack( Dst.Add( StmtNode{ std::move( Copy ) } ) );
        }
    }

    [[nodiscard]] StmtId EmitOneStmt ( StmtId Id )
    {
        StmtList Emitted;
        EvalStmt( Id, Emitted, false );
        return Emitted.IsEmpty() ? StmtId{} : Emitted[0];
    }

    void PushExpr ( StmtList &Out, ExprId Value, ::Volt::Core::SourceRange Loc )
    {
        if ( Value.IsValid() )
        {
            Out.PushBack( Dst.Add( StmtNode{ ExprStmt{ .Loc = Loc, .Expr = Value } } ) );
        }
    }

    [[nodiscard]] Symbol Reintern ( Symbol Handle ) const
    {
        return Handle.IsValid() ? Dst.Strings().Intern( Src.Text( Handle ) ) : Handle;
    }

    /// The reflected walk that carries a node across, child by child. Same
    /// shape as Detail::CloneField (AstClone.hpp) and for the same reason —
    /// a new AST node costs nothing here — except that expressions route
    /// through Emit and statement lists through the evaluator, so a loop
    /// nested inside emitted code still unrolls.
    template <typename NodeType> void MapFields ( NodeType &Node )
    {
        Meta::ForEachField( Node,
                            [&] ( std::string_view, auto &Field )
                            {
                                using FieldType = std::remove_cvref_t<decltype( Field )>;
                                if constexpr ( std::same_as<FieldType, ExprId> )
                                {
                                    Field = Emit( Field );
                                }
                                else if constexpr ( ListOf<FieldType, ExprId> )
                                {
                                    for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
                                    {
                                        Field[Index] = Emit( Field[Index] );
                                    }
                                }
                                else if constexpr ( std::same_as<FieldType, StmtId> )
                                {
                                    Field = EmitOneStmt( Field );
                                }
                                else if constexpr ( ListOf<FieldType, StmtId> )
                                {
                                    Field = EvalStmtList( Field );
                                }
                                else if constexpr ( std::same_as<FieldType, DeclId> )
                                {
                                    Field = CloneDecl( Src, Dst, Field );
                                }
                                else if constexpr ( ListOf<FieldType, DeclId> )
                                {
                                    for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
                                    {
                                        Field[Index] = CloneDecl( Src, Dst, Field[Index] );
                                    }
                                }
                                else if constexpr ( std::same_as<FieldType, TypeId> )
                                {
                                    Field = CloneType( Src, Dst, Field );
                                }
                                else if constexpr ( ListOf<FieldType, TypeId> )
                                {
                                    for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
                                    {
                                        Field[Index] = CloneType( Src, Dst, Field[Index] );
                                    }
                                }
                                else if constexpr ( std::same_as<FieldType, ParamId> )
                                {
                                    Field = CloneParam( Src, Dst, Field );
                                }
                                else if constexpr ( ListOf<FieldType, ParamId> )
                                {
                                    for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
                                    {
                                        Field[Index] = CloneParam( Src, Dst, Field[Index] );
                                    }
                                }
                                else if constexpr ( std::same_as<FieldType, Symbol> )
                                {
                                    Field = Reintern( Field );
                                }
                                else if constexpr ( ListOf<FieldType, Symbol> )
                                {
                                    for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
                                    {
                                        Field[Index] = Reintern( Field[Index] );
                                    }
                                }
                            } );
    }

    MacroEnv &Env;
    const AstContext &Src;
    AstContext &Dst;
    std::unordered_map<std::string, MacroValue> Bindings;
    MemoMap Memo;
    std::optional<MacroTypeDesc> SelfDesc;
    std::uint32_t Commands = 0;
};

} // namespace

namespace Volt::MiddleEnd::ConstEval
{

void EvalMacroBody ( MacroEnv &Env, const Frontend::StmtList &Body, Frontend::StmtList &Out, bool bTailValue )
{
    Evaluator Machine{ Env };
    Machine.Run( Body, Out, bTailValue );
}

} // namespace Volt::MiddleEnd::ConstEval
