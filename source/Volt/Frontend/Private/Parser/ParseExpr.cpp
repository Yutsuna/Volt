#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/Lexer/Lexer.hpp"
#include "Volt/Frontend/Parser/Parser.hpp"
#include "Volt/Frontend/Parser/Pratt.hpp"

#include <cstddef>
#include <cstdint>
#include <variant>

namespace
{

// Only these expression kinds denote something a trailing `do ... end`
// / `{ ... }` block can sensibly attach to (a callee); literals, hash
// literals, and the like can never be "called" with a block.
[[nodiscard]] bool IsBlockAttachable ( Volt::Frontend::ExprKind Kind )
{
    switch ( Kind )
    {
    case Volt::Frontend::ExprKind::Identifier:
    case Volt::Frontend::ExprKind::Member:
    case Volt::Frontend::ExprKind::Call:
    case Volt::Frontend::ExprKind::Index:
        return true;
    default:
        return false;
    }
}

} // namespace

bool Volt::Frontend::Parser::CanStartCommandArgument () const
{
    switch ( PeekKind() )
    {
    case TokenKind::StringLiteral:
    case TokenKind::CommandLiteral:
    case TokenKind::IntLiteral:
    case TokenKind::FloatLiteral:
    case TokenKind::CharLiteral:
    case TokenKind::SymbolLiteral:
    case TokenKind::InstanceVar:
    case TokenKind::IvarInterp:
    case TokenKind::Identifier:
    case TokenKind::Constant:
    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
    case TokenKind::KwNil:
    case TokenKind::KwSelf:
    case TokenKind::KwSuper:
        return true;
    default:
        return false;
    }
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParseExpr ( int MinBindingPower )
{
    ExprId Lhs = ParsePrefix();

    for ( ;; )
    {
        // Support multiline pipelines where |> or <| is at the start of a newline.
        std::size_t Ahead = 0;
        while ( PeekKind( Ahead ) == TokenKind::Newline )
        {
            ++Ahead;
        }
        if ( PeekKind( Ahead ) == TokenKind::PipeGreater or PeekKind( Ahead ) == TokenKind::LessPipe or
             PeekKind( Ahead ) == TokenKind::Shr )
        {
            SkipNewlines();
        }

        const TokenKind Op    = PeekKind();
        const BindingPower Bp = InfixBinding( Op );
        if ( Bp.Left == 0 or Bp.Left < MinBindingPower )
        {
            break;
        }

        const std::uint32_t Begin = Peek().Range.Begin;
        Advance(); // operator
        // A trailing infix operator continues the expression on the next
        // line (`"{" +` <newline> `"..."`); nothing else can legally
        // follow an operator, so this is unambiguous.
        SkipNewlines();

        if ( Op == TokenKind::Question )
        {
            Ternary Node;
            Node.Cond = Lhs;
            Node.Then = ParseExpr( 0 );
            Expect( TokenKind::Colon, "in ternary expression" );
            Node.Else = ParseExpr( Bp.Right );
            Lhs       = MakeExpr( Node, RangeSince( Begin ) );
        }
        else if ( IsAssignment( Op ) )
        {
            Assign Node;
            Node.Op     = Op;
            Node.Target = Lhs;
            Node.Value  = ParseExpr( Bp.Right );
            Lhs         = MakeExpr( Node, RangeSince( Begin ) );
        }
        else if ( Op == TokenKind::PipeGreater )
        {
            // `a |> f` — Lhs is the piped value, Rhs is the function it feeds.
            Pipeline Node;
            Node.Lhs = Lhs;
            Node.Rhs = ParseExpr( Bp.Right );
            Lhs      = MakeExpr( Node, RangeSince( Begin ) );
        }
        else if ( Op == TokenKind::LessPipe )
        {
            // `f <| a` — mirror of `|>`: the function is on the left, the
            // value being fed is on the right, so swap into the same
            // Pipeline shape (Lhs = value, Rhs = function) for lowering.
            Pipeline Node;
            Node.Rhs = Lhs;
            Node.Lhs = ParseExpr( Bp.Right );
            Lhs      = MakeExpr( Node, RangeSince( Begin ) );
        }
        else if ( Op == TokenKind::Shr )
        {
            SkipNewlines();
            const ExprId RhsId  = ParseExpr( Bp.Right );
            const ExprKind LhsK = KindOf( Context.Expr( Lhs ) );
            const ExprKind RhsK = KindOf( Context.Expr( RhsId ) );
            if ( LhsK == ExprKind::Section or LhsK == ExprKind::Lambda or LhsK == ExprKind::Composition or
                 RhsK == ExprKind::Section or RhsK == ExprKind::Lambda or RhsK == ExprKind::Composition )
            {
                Composition Node;
                Node.Lhs = Lhs;
                Node.Rhs = RhsId;
                Lhs      = MakeExpr( Node, RangeSince( Begin ) );
            }
            else
            {
                Binary Node;
                Node.Op  = Op;
                Node.Lhs = Lhs;
                Node.Rhs = RhsId;
                Lhs      = MakeExpr( Node, RangeSince( Begin ) );
            }
        }
        else
        {
            Binary Node;
            Node.Op  = Op;
            Node.Lhs = Lhs;
            Node.Rhs = ParseExpr( Bp.Right );
            Lhs      = MakeExpr( Node, RangeSince( Begin ) );
        }
    }

    return Lhs;
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParsePrefix ()
{
    const std::uint32_t Begin = Here();
    const TokenKind Kind      = PeekKind();

    if ( Kind == TokenKind::AmpDot )
    {
        Advance();
        Section Node;
        Node.Loc = RangeSince( Begin );

        const TokenKind TargetKind = PeekKind();
        if ( TargetKind == TokenKind::Identifier or TargetKind == TokenKind::Constant )
        {
            Node.Kind   = ESectionKind::InstanceMethod;
            Node.Target = InternText( Advance() );

            // Check for optional trailing args: &.prefix("usr_")
            if ( Check( TokenKind::LParen ) )
            {
                Advance();
                ExprList Args;
                SymbolList ArgNames;
                ParseCallArguments( Args, ArgNames, TokenKind::RParen );
                Expect( TokenKind::RParen, "to close section call arguments" );
                Node.Args = std::move( Args );
            }
        }
        else
        {
            // Operator section: &.+ 5 or &.* 2
            Node.Kind          = ESectionKind::Operator;
            const Token &OpTok = Advance();
            Node.Target        = InternText( OpTok );
            Node.Op            = OpTok.Kind;
            if ( CanStartCommandArgument() or Check( TokenKind::IntLiteral ) or Check( TokenKind::FloatLiteral ) or
                 Check( TokenKind::StringLiteral ) )
            {
                const ExprId ArgId = ParseExpr( 65 );
                if ( ArgId.IsValid() )
                {
                    Node.Args.PushBack( ArgId );
                }
            }
        }

        if ( Accept( TokenKind::Dot ) )
        {
            if ( Accept( TokenKind::Bang ) )
            {
                Node.bNegated = true;
            }
        }

        return ParsePostfix( MakeExpr( Node, RangeSince( Begin ) ) );
    }

    if ( Kind == TokenKind::Amp )
    {
        // Static capture: `&Math.square`, `&transform`, and — so that a
        // composed point-free chain needs no intermediate name — a
        // parenthesised expression, `&( ( &.+ 10 ) >> ( &.* 2 ) )`.
        if ( Pos + 1 < Tokens.size() and
             ( Tokens[Pos + 1].Kind == TokenKind::Identifier or Tokens[Pos + 1].Kind == TokenKind::Constant or
               Tokens[Pos + 1].Kind == TokenKind::LParen ) )
        {
            Advance();
            Section Node;
            Node.Loc        = RangeSince( Begin );
            Node.Kind       = ESectionKind::StaticCapture;
            Node.TargetExpr = ParseExpr( 90 );
            return ParsePostfix( MakeExpr( Node, RangeSince( Begin ) ) );
        }
    }

    // Every VOLT_PREFIX row of Pratt.inl lowers to a Unary node here; a
    // new prefix operator is one manifest line, never a new case.
    if ( IsPrefixOperator( Kind ) )
    {
        Advance();
        Unary Node;
        Node.Op      = Kind;
        Node.Operand = ParseExpr( PrefixBindingPower );
        return MakeExpr( Node, RangeSince( Begin ) );
    }

    // `*p` builds a Deref node, not a Unary — structural, so not in the manifest.
    if ( Kind == TokenKind::Star )
    {
        Advance();
        Deref Node;
        Node.Operand = ParseExpr( PrefixBindingPower );
        return MakeExpr( Node, RangeSince( Begin ) );
    }

    return ParsePrimary();
}

void Volt::Frontend::Parser::ParseRescueEnsure ( BeginExpr &Node )
{
    while ( Accept( TokenKind::KwRescue ) )
    {
        const std::uint32_t RescueBegin = Here();
        RescueClause RescueNode;
        if ( Check( TokenKind::Identifier ) )
        {
            RescueNode.VarName = InternText( Advance() );
            if ( Accept( TokenKind::Colon ) )
            {
                RescueNode.ExceptionType = ParseType();
            }
        }
        else if ( Accept( TokenKind::Colon ) )
        {
            RescueNode.ExceptionType = ParseType();
        }
        SkipTerminators();
        ParseStatementBlock( RescueNode.Body );
        Node.RescueClauses.PushBack( MakeStmt( RescueNode, RangeSince( RescueBegin ) ) );
    }

    if ( Accept( TokenKind::KwEnsure ) )
    {
        SkipTerminators();
        ParseStatementBlock( Node.EnsureBody );
    }
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParsePrimary ()
{
    const std::uint32_t Begin = Here();
    const TokenKind Kind      = PeekKind();

    // NOLINTBEGIN (bugprone-branch-clone)
    switch ( Kind )
    {
    case TokenKind::IntLiteral:
    {
        const Symbol Text = InternText( Advance() );
        return ParsePostfix( MakeExpr( IntLiteral{ {}, Text }, RangeSince( Begin ) ) );
    }

    case TokenKind::FloatLiteral:
    {
        const Symbol Text = InternText( Advance() );
        return ParsePostfix( MakeExpr( FloatLiteral{ {}, Text }, RangeSince( Begin ) ) );
    }

    case TokenKind::CharLiteral:
    {
        const Symbol Text = InternText( Advance() );
        return ParsePostfix( MakeExpr( CharLiteral{ {}, Text }, RangeSince( Begin ) ) );
    }

    case TokenKind::SymbolLiteral:
    {
        const Symbol Text = InternText( Advance() );
        return ParsePostfix( MakeExpr( SymbolLiteral{ {}, Text }, RangeSince( Begin ) ) );
    }

    case TokenKind::StringLiteral:
    {
        const Token Tok = Advance();
        return ParsePostfix( ParseStringLiteral( Tok ) );
    }

    case TokenKind::CommandLiteral:
    {
        const Token Tok = Advance();
        return ParsePostfix( ParseCommandLiteral( Tok ) );
    }

    case TokenKind::IvarInterp:
    {
        const Token Tok = Advance();
        return ParsePostfix( ParseIvarInterp( Tok ) );
    }

    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
    {
        Advance();
        BoolLiteral Node;
        Node.Value = ( Kind == TokenKind::KwTrue );
        return ParsePostfix( MakeExpr( Node, RangeSince( Begin ) ) );
    }

    case TokenKind::KwNil:
        Advance();
        return ParsePostfix( MakeExpr( NilLiteral{}, RangeSince( Begin ) ) );

    case TokenKind::KwSelf:
        Advance();
        return ParsePostfix( MakeExpr( SelfExpr{}, RangeSince( Begin ) ) );

    case TokenKind::KwSizeOf:
    {
        Advance();
        SizeOf Node;
        Node.Type = ParseType();
        return MakeExpr( Node, RangeSince( Begin ) );
    }

    // Every compile-time type predicate parses here, through one arm: same
    // shape as `sizeof`, and the token *is* which question was asked. A new
    // predicate is a row in TokenKind.inl plus a label on this case — no new
    // parselet, no new node.
    case TokenKind::KwTriviallyDestructible:
    {
        TypeTrait Node;
        Node.Trait = Peek().Kind;
        Advance();
        Node.Type = ParseType();
        return MakeExpr( Node, RangeSince( Begin ) );
    }

    case TokenKind::KwSuper:
    {
        Advance();
        const ExprId SuperNode = MakeExpr( SuperExpr{}, RangeSince( Begin ) );
        if ( Check( TokenKind::LParen ) )
        {
            Call Node;
            Node.Callee = SuperNode;
            Expect( TokenKind::LParen, "to open super arguments" );
            ParseCallArguments( Node.Args, Node.ArgNames, TokenKind::RParen );
            Expect( TokenKind::RParen, "to close super arguments" );
            return ParsePostfix( MakeExpr( Node, RangeSince( Begin ) ) );
        }
        return ParsePostfix( SuperNode );
    }

    case TokenKind::KwRaise:
    {
        Advance();
        ExprId Arg{};
        if ( CanStartCommandArgument() or
             not( Check( TokenKind::Newline ) or Check( TokenKind::Semicolon ) or Check( TokenKind::Eof ) or
                  Check( TokenKind::KwEnd ) or Check( TokenKind::KwRescue ) or Check( TokenKind::KwEnsure ) or
                  Check( TokenKind::KwElse ) or Check( TokenKind::KwElsif ) or Check( TokenKind::RParen ) or
                  Check( TokenKind::RBracket ) or Check( TokenKind::RBrace ) ) )
        {
            // A string/interp argument is sugar for `Exception.new(msg)`, but
            // the parser has no TypeStore to resolve the exception root
            // type's name — Sema's RaiseExpr handler performs that desugar
            // once the root is known (see @[Literal( RaiseExpr )]).
            Arg = ParseExpr( 0 );
        }
        RaiseExpr Node;
        Node.Exception = Arg;
        return MakeExpr( Node, RangeSince( Begin ) );
    }

    case TokenKind::KwBegin:
    {
        Advance();
        SkipTerminators();
        BeginExpr Node;
        ParseStatementBlock( Node.Body );
        ParseRescueEnsure( Node );
        Expect( TokenKind::KwEnd, "to close begin block" );
        return MakeExpr( Node, RangeSince( Begin ) );
    }

    case TokenKind::InstanceVar:
    {
        const Symbol Text = InternText( Advance() );
        return ParsePostfix( MakeExpr( InstanceVar{ {}, Text }, RangeSince( Begin ) ) );
    }

    case TokenKind::Identifier:
    {
        const Symbol Text  = InternText( Advance() );
        const ExprId Ident = MakeExpr( Identifier{ {}, Text }, RangeSince( Begin ) );
        // Paren-less command call: `raise "x"`, `divisible_by? 2`.
        if ( CanStartCommandArgument() )
        {
            return ParseCommandCallArgs( Ident, RangeSince( Begin ) );
        }
        return ParsePostfix( Ident );
    }

    case TokenKind::Constant:
    {
        const Symbol Text = InternText( Advance() );
        return ParsePostfix( MakeExpr( Identifier{ {}, Text }, RangeSince( Begin ) ) );
    }

    case TokenKind::LParen:
        return ParsePostfix( ParseParenOrGroup() );

    case TokenKind::LBracket:
        return ParsePostfix( ParseArrayLiteral() );

    case TokenKind::LBrace:
        return ParsePostfix( ParseHashLiteral() );

    case TokenKind::KwCase:
        return ParseCaseExpr();

    case TokenKind::KwIf:
        return ParseIf();

    case TokenKind::KwUnless:
        return ParseUnless();

    case TokenKind::Dot:
        return ParsePostfix( ParseDotCall() );

    case TokenKind::Lt:
        if ( AtJsxStart() )
        {
            return ParseJsxElement();
        }
        [[fallthrough]];

    default:
        ReportHere( "expected an expression" );
        Advance();
        return MakeExpr( NilLiteral{}, RangeSince( Begin ) );
    }
    // NOLINTEND (bugprone-branch-clone)
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParsePostfix ( ExprId Lhs )
{
    for ( ;; )
    {
        // A chain continues on the next line when that line opens with `.`:
        //
        //     clean = users
        //       .filter( ... )   # a comment here is fine: the lexer folds
        //       .map( ... )      # blank lines and comments into one Newline
        //
        // Without this, the tail links parse as separate statements — two
        // orphan DotCall nodes with an implicit `self` receiver, i.e. a
        // silently wrong program rather than a syntax error.
        //
        // `Dot` only. `LBracket` and `LParen` opening a line are an array
        // literal and a parenthesised expression, and `&.` (AmpDot) starts a
        // section — none of them may be swallowed as a continuation.
        if ( Check( TokenKind::Newline ) )
        {
            std::size_t Ahead = 0;
            while ( PeekKind( Ahead ) == TokenKind::Newline )
            {
                ++Ahead;
            }
            if ( PeekKind( Ahead ) != TokenKind::Dot )
            {
                break;
            }
            SkipNewlines();
        }

        const std::uint32_t Begin = Here();

        if ( Accept( TokenKind::Dot ) or Accept( TokenKind::ColonColon ) )
        {
            Member Node;
            Node.Object       = Lhs;
            const Token &Name = Peek();
            // A receiver trait (`obj.is_a? T`) stands exactly where a member
            // name stands and interns as its own spelling — it is a reserved
            // word, so the lexer hands it over as a keyword token rather than
            // an Identifier. Nothing downstream ever resolves the name:
            // ExprInferencer answers the question through
            // ConstEval::TraitEngine and replaces the node with its answer.
            const bool bTrait = IsReceiverTrait( PeekKind() );
            if ( Check( TokenKind::Identifier ) or Check( TokenKind::Constant ) or bTrait )
            {
                Node.Name = InternText( Advance() );
            }
            else
            {
                ReportHere( "expected a member name after '.'" );
                Node.Name = InternText( Name );
            }
            Lhs = MakeExpr( Node, RangeSince( Begin ) );

            // The paren-less spelling, `obj.is_a? T`. **Only** a trait absorbs
            // an argument this way — `a.b c` still parses as a member access
            // followed by a separate statement, so no existing program moves.
            // A parenthesised `obj.is_a?( T )` needs nothing here: the loop's
            // own `LParen` arm builds the identical Call on its next turn.
            //
            // The operand is a type name or a symbol, never an arithmetic
            // expression, so it is parsed above every infix operator that can
            // legally follow the question — `x.is_a? T and y` groups as
            // `( x.is_a? T ) and y`, which is the only reading anyone means.
            // 65 is the operator-section arm's own choice, for the same reason.
            if ( bTrait and not Check( TokenKind::LParen ) and CanStartCommandArgument() )
            {
                Call TraitCall;
                TraitCall.Callee = Lhs;
                TraitCall.Args.PushBack( ParseExpr( 65 ) );
                TraitCall.ArgNames.PushBack( Symbol{} );
                Lhs = MakeExpr( TraitCall, RangeSince( Begin ) );
            }
        }
        else if ( AtGenericArgs() )
        {
            // Explicit instantiation: `Vector<T>.new`, `alloc<T>( n )`.
            GenericInst Node;
            Node.Base = Lhs;
            Node.Args = ParseGenericArgs();
            Lhs       = MakeExpr( Node, RangeSince( Begin ) );
        }
        else if ( Accept( TokenKind::LParen ) )
        {
            Call Node;
            Node.Callee = Lhs;
            ParseCallArguments( Node.Args, Node.ArgNames, TokenKind::RParen );
            Expect( TokenKind::RParen, "to close call arguments" );
            PromoteCapturedBlock( Node );
            Lhs = MakeExpr( Node, RangeSince( Begin ) );
        }
        else if ( Accept( TokenKind::LBracket ) )
        {
            Index Node;
            Node.Object = Lhs;
            SymbolList Ignored;
            ParseCallArguments( Node.Args, Ignored, TokenKind::RBracket );
            Expect( TokenKind::RBracket, "to close index" );
            Lhs = MakeExpr( Node, RangeSince( Begin ) );
        }
        else if ( ( ( Check( TokenKind::KwDo ) and not bNoDoBlock ) or Check( TokenKind::LBrace ) ) and
                  IsBlockAttachable( KindOf( Context.Expr( Lhs ) ) ) )
        {
            // Trailing `do |x| ... end` / `{ |x| ... }` block — same
            // construct, two spellings.
            Lhs = AttachTrailingBlock( Lhs, ParseDoBlock(), Begin );
        }
        else if ( Check( TokenKind::PlusPlus ) or Check( TokenKind::MinusMinus ) )
        {
            const TokenKind Op = Advance().Kind;
            Postfix Node;
            Node.Op      = Op;
            Node.Operand = Lhs;
            Lhs          = MakeExpr( Node, RangeSince( Begin ) );
        }
        else
        {
            break;
        }
    }
    return Lhs;
}

void Volt::Frontend::Parser::ParseCallArguments ( ExprList &Args, SymbolList &ArgNames, TokenKind Close )
{
    SkipNewlines();
    if ( Check( Close ) )
    {
        return;
    }

    // Delimited by parens/brackets: a fresh context where `do` may
    // attach again (`foo(x.each do ... end)`).
    const bool Saved = bNoDoBlock;
    bNoDoBlock       = false;
    do
    {
        SkipNewlines();
        if ( Check( Close ) ) // tolerate a trailing comma
        {
            break;
        }
        Symbol Name;
        if ( Check( TokenKind::Identifier ) and PeekKind( 1 ) == TokenKind::Colon )
        {
            Name = InternText( Advance() );
            Advance(); // ':'
        }
        Args.PushBack( ParseExpr( 0 ) );
        ArgNames.PushBack( Name );
        SkipNewlines();
    } while ( Accept( TokenKind::Comma ) );
    bNoDoBlock = Saved;
    SkipNewlines();
}

void Volt::Frontend::Parser::PromoteCapturedBlock ( Call &Node )
{
    if ( Node.BlockArg.IsValid() or Node.Args.IsEmpty() )
    {
        return;
    }

    // Only the trailing argument, and only an unnamed one: `f( fn: &g )`
    // names a positional slot on purpose, and a capture in the middle of
    // the list is an ordinary Proc value.
    const std::size_t Last = Node.Args.Size() - 1;
    if ( Node.ArgNames[Last].IsValid() )
    {
        return;
    }

    const ExprKind Kind = KindOf( Context.Expr( Node.Args[Last] ) );
    if ( Kind != ExprKind::Section and Kind != ExprKind::Composition and Kind != ExprKind::Lambda )
    {
        return;
    }

    Node.BlockArg = Node.Args[Last];

    ExprList Args;
    SymbolList Names;
    for ( std::size_t Index = 0; Index < Last; ++Index )
    {
        Args.PushBack( Node.Args[Index] );
        Names.PushBack( Node.ArgNames[Index] );
    }
    Node.Args     = std::move( Args );
    Node.ArgNames = std::move( Names );
}

Volt::Frontend::ExprId Volt::Frontend::Parser::AttachTrailingBlock ( ExprId Lhs, ExprId BlockId, std::uint32_t Begin )
{
    // Reach into the arena and mutate the existing Call in place when
    // possible (`each do`, `v.each do`) rather than nesting a wrapper
    // Call around it — this is the one deliberate exception to the
    // value-AST convention that nodes are replaced via Context.Add, not
    // edited through a live reference. It is safe because no further
    // Context.Add call happens between the lookup and the assignment.
    if ( Call *AsCall = std::get_if<Call>( &Context.Expr( Lhs ) ); AsCall != nullptr and not AsCall->BlockArg.IsValid() )
    {
        AsCall->BlockArg = BlockId;
        return Lhs;
    }

    // A bare callee (e.g. a block on a local/identifier) has no Call to
    // mutate — wrap it in one.
    Call Node;
    Node.Callee   = Lhs;
    Node.BlockArg = BlockId;
    return MakeExpr( Node, RangeSince( Begin ) );
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParseDoBlock ()
{
    const std::uint32_t Begin = Here();

    // `do ... end` and `{ ... }` are the same block literal, just
    // spelled with different delimiters (Ruby/Crystal).
    const bool bBrace = Check( TokenKind::LBrace );
    if ( bBrace )
    {
        Advance();
    }
    else
    {
        Expect( TokenKind::KwDo, "to begin a block" );
    }

    Block Node;
    // Parameters are hard-expected between pipes, outside the Pratt
    // table, so `|` never reads as the bitwise-or operator here.
    if ( Accept( TokenKind::Pipe ) )
    {
        do
        {
            Param ParamNode;
            ParamNode.Name = InternText( Expect( TokenKind::Identifier, "as a block parameter name" ) );
            if ( Accept( TokenKind::Colon ) )
            {
                ParamNode.DeclType = ParseType();
            }
            Node.Params.PushBack( Context.Add( ParamNode ) );
        } while ( Accept( TokenKind::Comma ) );
        Expect( TokenKind::Pipe, "to close block parameters" );
    }

    // The body is a fresh statement context: `do` may attach again.
    const bool Saved = bNoDoBlock;
    bNoDoBlock       = false;

    // `end` is already one of ParseStatementBlock's built-in terminators;
    // only a brace block needs the extra one.
    const TokenKind Close = bBrace ? TokenKind::RBrace : TokenKind::KwEnd;
    ParseStatementBlock( Node.Body, Close );

    bNoDoBlock = Saved;
    Expect( Close, "to close block" );

    return MakeExpr( Node, RangeSince( Begin ) );
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParseCaseExpr ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::KwCase, "to open a case expression" );

    ExprId Target{};
    SkipNewlines();

    if ( not Check( TokenKind::KwWhen ) and not Check( TokenKind::KwEnd ) )
    {
        Target = ParseExpr( 0 );
        SkipTerminators();
    }

    StmtList Clauses;
    StmtList ElseBody;

    while ( Check( TokenKind::KwWhen ) )
    {
        const std::uint32_t WhenBegin = Here();
        Advance(); // consume `when`

        WhenClause Clause;
        do
        {
            SkipNewlines();
            Clause.Patterns.PushBack( ParseExpr( 0 ) );
            SkipNewlines();
        } while ( Accept( TokenKind::Comma ) );

        Accept( TokenKind::KwThen ); // optional `then` after `when` patterns
        SkipTerminators();

        ParseStatementBlock( Clause.Body, TokenKind::KwWhen );

        const StmtId ClauseId = MakeStmt( Clause, RangeSince( WhenBegin ) );
        Clauses.PushBack( ClauseId );
    }

    if ( Accept( TokenKind::KwElse ) )
    {
        ParseStatementBlock( ElseBody );
    }

    Expect( TokenKind::KwEnd, "to close a case expression" );

    CaseExpr Node;
    Node.Target   = Target;
    Node.Clauses  = std::move( Clauses );
    Node.ElseBody = std::move( ElseBody );

    return MakeExpr( Node, RangeSince( Begin ) );
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParseDotCall ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::Dot, "to start a dot-call shorthand" );

    DotCall Node;
    if ( Check( TokenKind::Identifier ) or Check( TokenKind::Constant ) )
    {
        Node.Method = InternText( Advance() );
    }
    else
    {
        ReportHere( "expected a method name after '.' in dot-call" );
    }

    if ( Accept( TokenKind::LParen ) )
    {
        ParseCallArguments( Node.Args, Node.ArgNames, TokenKind::RParen );
        Expect( TokenKind::RParen, "to close dot-call arguments" );
    }
    else if ( CanStartCommandArgument() )
    {
        const bool Saved = bNoDoBlock;
        bNoDoBlock       = true;
        do
        {
            Symbol Name;
            if ( Check( TokenKind::Identifier ) and PeekKind( 1 ) == TokenKind::Colon )
            {
                Name = InternText( Advance() );
                Advance(); // ':'
            }
            Node.Args.PushBack( ParseExpr( 0 ) );
            Node.ArgNames.PushBack( Name );
        } while ( Accept( TokenKind::Comma ) );
        bNoDoBlock = Saved;
    }

    return MakeExpr( Node, RangeSince( Begin ) );
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParseCommandCallArgs ( ExprId Callee, Core::SourceRange Start )
{
    Call Node;
    Node.Callee = Callee;

    // Inside the argument list a trailing `do` belongs to this command
    // call, not to the last argument — suppress it there (`{` still
    // binds tightest and may attach to an argument, as in Ruby).
    const bool Saved = bNoDoBlock;
    bNoDoBlock       = true;
    do
    {
        Symbol Name;
        if ( Check( TokenKind::Identifier ) and PeekKind( 1 ) == TokenKind::Colon )
        {
            Name = InternText( Advance() );
            Advance(); // ':'
        }
        Node.Args.PushBack( ParseExpr( 0 ) );
        Node.ArgNames.PushBack( Name );
    } while ( Accept( TokenKind::Comma ) );
    bNoDoBlock = Saved;

    // This result never flows back through ParsePostfix, so claim the
    // trailing block here (`foo 1 do |y| ... end`).
    if ( Check( TokenKind::KwDo ) or Check( TokenKind::LBrace ) )
    {
        Node.BlockArg = ParseDoBlock();
    }
    PromoteCapturedBlock( Node );

    return MakeExpr( Node, RangeSince( Start.Begin ) );
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParseParenOrGroup ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::LParen, "to open a parenthesised expression" );
    SkipNewlines();

    // Disambiguate lambda parameter list `(x : T, y) => ...` or `() => ...` vs grouped expression `(x + 1)`
    bool bIsLambda        = false;
    std::size_t Lookahead = Pos;
    if ( Lookahead < Tokens.size() and Tokens[Lookahead].Kind == TokenKind::RParen )
    {
        if ( Lookahead + 1 < Tokens.size() and Tokens[Lookahead + 1].Kind == TokenKind::FatArrow )
        {
            bIsLambda = true;
        }
    }
    else
    {
        while ( Lookahead < Tokens.size() and Tokens[Lookahead].Kind != TokenKind::RParen and
                Tokens[Lookahead].Kind != TokenKind::Eof )
        {
            ++Lookahead;
        }
        if ( Lookahead < Tokens.size() and Tokens[Lookahead].Kind == TokenKind::RParen )
        {
            if ( Lookahead + 1 < Tokens.size() and Tokens[Lookahead + 1].Kind == TokenKind::FatArrow )
            {
                bIsLambda = true;
            }
        }
    }

    if ( bIsLambda )
    {
        ParamList Params;
        ParseParameterList( Params );
        Expect( TokenKind::RParen, "to close lambda parameters" );
        SkipNewlines();
        Expect( TokenKind::FatArrow, "to open lambda body" );
        SkipNewlines();

        const ExprId Body = ParseExpr( 0 );

        Lambda LNode;
        LNode.Loc    = RangeSince( Begin );
        LNode.Params = std::move( Params );
        LNode.Body   = Body;
        return MakeExpr( LNode, RangeSince( Begin ) );
    }

    // Parens delimit a fresh expression context: `do` may attach again.
    const bool Saved   = bNoDoBlock;
    bNoDoBlock         = false;
    const ExprId Inner = ParseExpr( 0 );
    bNoDoBlock         = Saved;
    SkipNewlines();

    // `( Value : Type )` — explicit type ascription, not a cast (see
    // `TypedExpr` in `Nodes.inl`): distinguished from lambda params above by
    // running only once `Inner` is already a full expression.
    if ( Accept( TokenKind::Colon ) )
    {
        SkipNewlines();
        const TypeId Annotation = ParseType();
        SkipNewlines();
        Expect( TokenKind::RParen, "to close a type-ascribed expression" );

        TypedExpr Node;
        Node.Loc   = RangeSince( Begin );
        Node.Value = Inner;
        Node.Type  = Annotation;
        return MakeExpr( Node, RangeSince( Begin ) );
    }

    Expect( TokenKind::RParen, "to close a parenthesised expression" );
    return Inner;
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParseArrayLiteral ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::LBracket, "to open an array literal" );

    ArrayLit Node;
    SkipNewlines();
    if ( not Check( TokenKind::RBracket ) )
    {
        do
        {
            SkipNewlines();
            if ( Check( TokenKind::RBracket ) )
            {
                break;
            }
            Node.Elements.PushBack( ParseExpr( 0 ) );
            SkipNewlines();
        } while ( Accept( TokenKind::Comma ) );
    }
    Expect( TokenKind::RBracket, "to close an array literal" );

    if ( Accept( TokenKind::KwOf ) )
    {
        Node.ElemType = ParseType();
    }
    return MakeExpr( Node, RangeSince( Begin ) );
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParseHashLiteral ()
{
    const std::uint32_t Begin = Here();
    Expect( TokenKind::LBrace, "to open a hash literal or block expression" );

    // Disambiguate block `{ expr }` / `{ stmt; stmt }` vs `{ key => value }` hash literal.
    // If empty `{}` -> HashLiteral.
    SkipNewlines();
    if ( Check( TokenKind::RBrace ) )
    {
        Advance();
        return MakeExpr( HashLit{}, RangeSince( Begin ) );
    }

    // Lookahead: parse key expression. If next token is NOT FatArrow `=>`, this is a block `{ expr }`.
    std::size_t Lookahead = Pos;
    bool bHasFatArrow     = false;
    while ( Lookahead < Tokens.size() and Tokens[Lookahead].Kind != TokenKind::RBrace and
            Tokens[Lookahead].Kind != TokenKind::Newline and Tokens[Lookahead].Kind != TokenKind::Eof )
    {
        if ( Tokens[Lookahead].Kind == TokenKind::FatArrow )
        {
            bHasFatArrow = true;
            break;
        }
        ++Lookahead;
    }

    if ( not bHasFatArrow )
    {
        // Single block expression or statement list: `{ expr }`
        const bool Saved  = bNoDoBlock;
        bNoDoBlock        = false;
        const ExprId Body = ParseExpr( 0 );
        bNoDoBlock        = Saved;
        SkipNewlines();
        Expect( TokenKind::RBrace, "to close block expression" );
        return Body;
    }

    HashLit Node;
    do
    {
        SkipNewlines();
        if ( Check( TokenKind::RBrace ) )
        {
            break;
        }
        Node.Keys.PushBack( ParseExpr( InfixBinding( TokenKind::FatArrow ).Left + 1 ) );
        Expect( TokenKind::FatArrow, "between hash key and value" );
        Node.Values.PushBack( ParseExpr( 0 ) );
        SkipNewlines();
    } while ( Accept( TokenKind::Comma ) );
    Expect( TokenKind::RBrace, "to close a hash literal" );

    if ( Accept( TokenKind::KwOf ) )
    {
        Node.KeyType = ParseType();
        Expect( TokenKind::FatArrow, "between hash key and value types" );
        Node.ValueType = ParseType();
    }
    return MakeExpr( Node, RangeSince( Begin ) );
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParseStringLiteral ( const Token &Tok )
{
    if ( not Tok.bHasInterpolation )
    {
        StringLiteral Node;
        Node.Value = Tok.Lexeme;
        return MakeExpr( Node, Tok.Range );
    }

    Interp Node;
    ParseInterpolationParts( Tok, Node.Parts );
    return MakeExpr( Node, Tok.Range );
}

// `` `cmd` `` — the same segments a string is built from, wrapped in the node
// that says this text is a command to run rather than a value to concatenate.
// A command with no interpolation still gets its single literal chunk, so
// ConstEval reads Parts one way in both cases.
Volt::Frontend::ExprId Volt::Frontend::Parser::ParseCommandLiteral ( const Token &Tok )
{
    CommandLit Node;
    if ( Tok.bHasInterpolation )
    {
        ParseInterpolationParts( Tok, Node.Parts );
    }
    else
    {
        StringLiteral Chunk;
        Chunk.Value = Tok.Lexeme;
        Node.Parts.PushBack( MakeExpr( Chunk, Tok.Range ) );
    }
    return MakeExpr( Node, Tok.Range );
}

// `@#{ expr }` — the lexeme is the inner expression text, re-parsed exactly as
// an interpolated string re-parses its own splices. The `+ 3` skips `@#{`, so
// a diagnostic inside the hole points at the hole.
Volt::Frontend::ExprId Volt::Frontend::Parser::ParseIvarInterp ( const Token &Tok )
{
    IvarInterp Node;
    Node.Name =
        ParseSubExpression( Interner.Resolve( Tok.Lexeme ), Tok.Range, static_cast<std::uint32_t>( Tok.Range.Begin + 3 ) );
    return MakeExpr( Node, Tok.Range );
}

void Volt::Frontend::Parser::ParseInterpolationParts ( const Token &Tok, ExprList &Parts )
{
    const std::string_view Raw = Interner.Resolve( Tok.Lexeme );

    std::size_t LiteralStart = 0;
    std::size_t Index        = 0;
    while ( Index < Raw.size() )
    {
        if ( Raw[Index] == '#' and Index + 1 < Raw.size() and Raw[Index + 1] == '{' )
        {
            if ( Index > LiteralStart )
            {
                StringLiteral Chunk;
                Chunk.Value = Interner.Intern( Raw.substr( LiteralStart, Index - LiteralStart ) );
                Parts.PushBack( MakeExpr( Chunk, Tok.Range ) );
            }

            std::size_t Cursor = Index + 2;
            int Depth          = 1;
            while ( Cursor < Raw.size() and Depth > 0 )
            {
                if ( Raw[Cursor] == '{' )
                {
                    ++Depth;
                }
                else if ( Raw[Cursor] == '}' )
                {
                    --Depth;
                    if ( Depth == 0 )
                    {
                        break;
                    }
                }
                ++Cursor;
            }

            const std::string_view ExprText = Raw.substr( Index + 2, Cursor - ( Index + 2 ) );
            // Raw is a verbatim slice of the file starting one byte past the
            // opening quote, so a fragment offset maps back exactly.
            const auto Base = static_cast<std::uint32_t>( Tok.Range.Begin + 1 + Index + 2 );
            Parts.PushBack( ParseSubExpression( ExprText, Tok.Range, Base ) );

            Index        = Cursor + 1;
            LiteralStart = Index;
        }
        else
        {
            ++Index;
        }
    }

    if ( LiteralStart < Raw.size() )
    {
        StringLiteral Chunk;
        Chunk.Value = Interner.Intern( Raw.substr( LiteralStart ) );
        Parts.PushBack( MakeExpr( Chunk, Tok.Range ) );
    }
}

Volt::Frontend::ExprId
Volt::Frontend::Parser::ParseSubExpression ( std::string_view Text, Core::SourceRange Range, std::uint32_t BaseOffset )
{
    // The sub-lexer numbers the fragment from zero, so every node it builds
    // would claim to live at the very start of the file. The fragment is a
    // verbatim slice of the source, so sliding the new nodes by its absolute
    // offset restores each one's real position.
    const ArenaMark Mark = MarkArenas( Context );

    Lexer SubLexer( Context.FileId(), Text, Interner, Diagnostics );
    std::vector<Token> SubTokens = SubLexer.Tokenize();
    Parser SubParser( std::move( SubTokens ), Context, Diagnostics );
    const ExprId Result = SubParser.ParseExpr( 0 );

    ShiftLocsSince( Context, Mark, BaseOffset );

    if ( Result.IsValid() )
    {
        return Result;
    }
    return MakeExpr( NilLiteral{}, Range );
}

Volt::Frontend::ExprId Volt::Frontend::Parser::ParseExpression ()
{
    SkipNewlines();
    return ParseExpr( 0 );
}
