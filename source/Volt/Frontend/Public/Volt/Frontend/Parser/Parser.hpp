#pragma once

#include "Frontend_export.hpp"
#include "Volt/Core/Diagnostics/DiagEngine.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Volt
{

namespace Frontend
{

    /// Recursive-descent parser with a Pratt expression core. Consumes a
    /// token stream and populates a single AstContext. Newlines are
    /// significant terminators; the parser is resilient (reports, then
    /// recovers to the next statement) so one error does not cascade.
    class FRONTEND_EXPORT Parser
    {

    public:

        Parser ( std::vector<Token> InTokens,
                 AstContext &InContext,
                 Core::DiagEngine::Bag &InDiagnostics,
                 std::string_view InSource = {} );

        /// Parse a whole file (mixed top-level declarations and statements).
        void ParseFile ();

        /// Parse a `.vlx` component file (JSX-aware top level).
        void ParseComponentFile ();

        /// Parse a run of member declarations until Eof. Entry point for
        /// re-parsing macro-generated text into an enclosing decl body.
        void ParseMemberBlock ( DeclList &Out );

        /// Parse one expression until Eof. Entry point for evaluating the
        /// contents of a macro template tag at expansion time.
        [[nodiscard]] ExprId ParseExpression ();

    private:

        // --- Token cursor ------------------------------------------------

        [[nodiscard]] const Token &Peek ( std::size_t Ahead = 0 ) const
        {
            const std::size_t Index = Pos + Ahead;
            return Index < Tokens.size() ? Tokens[Index] : Tokens.back();
        }

        [[nodiscard]] TokenKind PeekKind ( std::size_t Ahead = 0 ) const
        {
            return Peek( Ahead ).Kind;
        }

        [[nodiscard]] bool Check ( TokenKind Kind ) const
        {
            return PeekKind() == Kind;
        }

        [[nodiscard]] bool AtEnd () const
        {
            return Check( TokenKind::Eof );
        }

        const Token &Advance ()
        {
            const Token &Current = Peek();
            if ( !AtEnd() )
            {
                ++Pos;
            }
            return Current;
        }

        bool Accept ( TokenKind Kind )
        {
            if ( Check( Kind ) )
            {
                Advance();
                return true;
            }
            return false;
        }

        const Token &Expect ( TokenKind Kind, std::string_view Where );

        void SkipNewlines ()
        {
            while ( Check( TokenKind::Newline ) )
            {
                Advance();
            }
        }

        void SkipTerminators ()
        {
            while ( Check( TokenKind::Newline ) or Check( TokenKind::Semicolon ) )
            {
                Advance();
            }
        }

        [[nodiscard]] std::string_view Spelling ( const Token &Tok ) const;

        /// Byte offset where the next token begins (for range start).
        [[nodiscard]] std::uint32_t Here () const
        {
            return Peek().Range.Begin;
        }

        /// Range spanning from Begin to the end of the last consumed token.
        [[nodiscard]] Core::SourceRange RangeSince ( std::uint32_t Begin ) const
        {
            const std::uint32_t End = Pos > 0 ? Tokens[Pos - 1].Range.End : Begin;
            return Core::SourceRange{ Context.FileId(), Begin, End };
        }

        // --- Diagnostics / recovery --------------------------------------

        void ReportHere ( std::string Message );
        void ReportAt ( Core::SourceRange Range, std::string Message );
        void RecoverToStatement ();

        // --- Node construction helpers -----------------------------------

        template <typename Node> [[nodiscard]] ExprId MakeExpr ( Node Value, Core::SourceRange Range )
        {
            Value.Loc = Range;
            return Context.Add( ExprNode{ std::move( Value ) } );
        }

        template <typename Node> [[nodiscard]] StmtId MakeStmt ( Node Value, Core::SourceRange Range )
        {
            Value.Loc = Range;
            return Context.Add( StmtNode{ std::move( Value ) } );
        }

        template <typename Node> [[nodiscard]] DeclId MakeDecl ( Node Value, Core::SourceRange Range )
        {
            Value.Loc = Range;
            return Context.Add( DeclNode{ std::move( Value ) } );
        }

        template <typename Node> [[nodiscard]] TypeId MakeType ( Node Value, Core::SourceRange Range )
        {
            Value.Loc = Range;
            return Context.Add( TypeNode{ std::move( Value ) } );
        }

        [[nodiscard]] Symbol InternText ( const Token &Tok ) const;

        // --- Grammar: expressions (ParseExpr.cpp) ------------------------

        [[nodiscard]] ExprId ParseExpr ( int MinBindingPower = 0 );
        [[nodiscard]] ExprId ParsePrefix ();
        [[nodiscard]] ExprId ParsePrimary ();
        [[nodiscard]] ExprId ParsePostfix ( ExprId Lhs );
        [[nodiscard]] ExprId ParseParenOrGroup ();
        [[nodiscard]] ExprId ParseArrayLiteral ();
        [[nodiscard]] ExprId ParseHashLiteral ();
        [[nodiscard]] ExprId ParseStringLiteral ( const Token &Tok );
        [[nodiscard]] ExprId ParseCommandLiteral ( const Token &Tok );
        [[nodiscard]] ExprId ParseIvarInterp ( const Token &Tok );
        // The segment scan shared by `"..."` and `` `...` ``: literal chunks
        // and `#{ ... }` splices, in source order. Written once because the two
        // literals differ only in the node they end up wrapped in.
        void ParseInterpolationParts ( const Token &Tok, ExprList &Parts );
        [[nodiscard]] ExprId ParseSubExpression ( std::string_view Text, Core::SourceRange Range, std::uint32_t BaseOffset );
        [[nodiscard]] ExprId ParseCommandCallArgs ( ExprId Callee, Core::SourceRange Start );
        [[nodiscard]] ExprId ParseDoBlock ();
        [[nodiscard]] ExprId ParseCaseExpr ();
        // Shared `while rescue ... end` + optional `ensure ... end` tail,
        // used by both the `begin` expression and a method's implicit
        // rescue. The caller has already filled `Node.Body`; this fills
        // `Node.RescueClauses` and `Node.EnsureBody` in place.
        void ParseRescueEnsure ( BeginExpr &Node );
        [[nodiscard]] ExprId ParseDotCall ();
        // Attach a trailing block to the call it follows: mutates `Lhs` in
        // place if it is already a bare `Call` with no block yet (`each do`),
        // otherwise wraps it in a new `Call` (e.g. a bare identifier callee).
        [[nodiscard]] ExprId AttachTrailingBlock ( ExprId Lhs, ExprId BlockId, std::uint32_t Begin );
        void ParseCallArguments ( ExprList &Args, SymbolList &ArgNames, TokenKind Close );

        // `f( &g )` writes the capture inside the argument list but means it
        // for the block slot: `&` marks a callable, and at a call site a
        // marked trailing argument fills `&block` — the same rule as Ruby's
        // `f( &blk )`. Deciding it here, from what was written, keeps Sema
        // free of any guess about which argument "looks like" a block.
        void PromoteCapturedBlock ( Call &Node );
        [[nodiscard]] bool CanStartCommandArgument () const;

        // --- Grammar: statements (ParseStmt.cpp) -------------------------

        [[nodiscard]] StmtId ParseStatement ();
        // If is an expression node (Expr.hpp), so these return an ExprId and
        // are reached from ParsePrimary; ParseStatement wraps the result in an
        // ExprStmt through its default arm.
        [[nodiscard]] ExprId ParseIf ();
        [[nodiscard]] ExprId ParseElsif ();
        [[nodiscard]] ExprId ParseUnless ();
        void ParseConditionalTail ( If &Node, const char *CloseHint );
        [[nodiscard]] StmtId ParseWhile ();
        [[nodiscard]] StmtId ParseUntil ();
        [[nodiscard]] StmtId ParseFor ();
        [[nodiscard]] StmtId ParseReturn ();
        [[nodiscard]] StmtId ParseBreak ();
        [[nodiscard]] StmtId ParseNext ();
        [[nodiscard]] StmtId ParseExprOrLocalStatement ();
        // `ExtraTerminator` lets a caller stop the block on a token besides
        // `end`/`else`/`elsif` (e.g. `}` for a brace block). `Eof` is a safe
        // no-op default: the loop already exits via AtEnd() before Check()
        // could ever see it.
        void ParseStatementBlock ( StmtList &Out, TokenKind ExtraTerminator = TokenKind::Eof );
        [[nodiscard]] StmtId ApplyModifiers ( StmtId Inner );

        /// Lifts an expression into statement position.
        [[nodiscard]] StmtId WrapExpr ( ExprId Expr, Core::SourceRange Span );

        /// `not Cond` — the single spelling of a negated condition, shared by
        /// `unless` and `until` in both their block and modifier forms. `not`
        /// rather than `!` because that is what `Bool` declares abstract
        /// (source/Lib/Primitives/Bool.vl), so it is the one the backend
        /// supplies directly instead of calling a Volt body.
        [[nodiscard]] ExprId NegateCond ( ExprId Cond, Core::SourceRange Span );

        /// True when Inner is an ExprStmt wrapping a `begin ... end` block —
        /// the one shape that makes `stmt while/until cond` a post-test loop.
        [[nodiscard]] bool WrapsBeginBlock ( StmtId Inner ) const;

        // --- Grammar: declarations (ParseDecl.cpp) -----------------------

        [[nodiscard]] bool AtDeclaration () const;
        [[nodiscard]] DeclId ParseDeclaration ();
        [[nodiscard]] DeclId ParseModule ();
        [[nodiscard]] DeclId ParseClass ();
        [[nodiscard]] DeclId ParseStruct ();
        [[nodiscard]] DeclId ParseEnum ();
        [[nodiscard]] DeclId ParseEnumCase ();
        void ParseEnumBody ( DeclList &Out );
        [[nodiscard]] DeclId ParseMixin ();
        [[nodiscard]] DeclId ParseMethod ( bool bAbstract, bool bExternal );
        [[nodiscard]] DeclId ParseInclude ();
        [[nodiscard]] DeclId ParseComponent ();
        [[nodiscard]] DeclId ParseCircuit ();
        [[nodiscard]] DeclId ParseAnnotation ();

        // `@[Primitive( "i32", 32 ), Literal( IntLiteral )]` is a *group*: one
        // `Annotation` decl per entry, so consumers keep matching a single
        // shape. ParseAnnotation returns the first and parks the rest here;
        // every declaration-collecting loop drains them right after.
        DeclList AnnotationOverflow;
        void DrainAnnotations ( DeclList &Out );
        void DrainAnnotations ( std::vector<DeclId> &Out );
        [[nodiscard]] DeclId ParseMacro ();
        [[nodiscard]] DeclId ParseMacroDef ();
        [[nodiscard]] DeclId ParseMacroBlock ();
        [[nodiscard]] DeclId ParseFieldOrMember ();
        void ParseDeclBlock ( DeclList &Out );

        // Consume a leading `private` / `protected` / `public` at the top of
        // one iteration of a declaration-body loop and decide what it governs.
        //
        // Standing alone on its line it is a *section marker*: `Section` moves
        // to it and every member below inherits that, until the next marker or
        // the body's `end`. With a declaration behind it on the same line it is
        // a one-member prefix and `Section` is left untouched. Either way
        // `MemberVisibility` is left holding what the member parsed next must
        // record, which is how `ParseMethod`/`ParseFieldOrMember` learn it
        // without every declaration entry point growing a parameter.
        //
        // Returns false when the keyword stood alone — the caller has no
        // member to parse this iteration and should go round again.
        bool TakeMemberVisibility ( EVisibility &Section );

        // What the next Field/Method declaration records, maintained entirely
        // by TakeMemberVisibility. A body saves and restores it around its own
        // loop, so a nested `class` inside a `private` section opens back at
        // the default rather than inheriting the enclosing section.
        EVisibility MemberVisibility = EVisibility::None;

        void ParseParameterList ( ParamList &Out );
        [[nodiscard]] GenericParamList ParseGenericParams ();

        // --- Grammar: types (ParseType.cpp) ------------------------------

        [[nodiscard]] TypeId ParseType ();
        [[nodiscard]] TypeId ParseTypePrimary ();
        [[nodiscard]] bool AtTypeStart () const;

        // --- Generic `<...>` lists ---------------------------------------
        // `<>` is shared by generics, JSX and comparison, so the three
        // contexts are told apart positionally rather than by the lexer:
        //   - type context   — unambiguous, a bare `Check(Lt)` is enough;
        //   - declarations   — `AtGenericOpen`, the no-space rule, so
        //                      `class Vector<T> < Base` reads both `<`s right;
        //   - expressions    — `AtGenericArgs`, which additionally scans for
        //                      the matching `>` so `a<B && c>d` stays a
        //                      comparison.
        [[nodiscard]] TypeList ParseGenericArgs ();
        // True when a `<` is glued to the token before it (no whitespace).
        [[nodiscard]] bool AtGenericOpen () const;
        // AtGenericOpen plus a lookahead proving a well-formed type list
        // follows. Used where `<` could still be the comparison operator.
        [[nodiscard]] bool AtGenericArgs () const;
        // Consume the closing `>`. A `>>` closes two nesting levels
        // (`Pointer<Vector<T>>`): it is split in place into two `Gt`s, the
        // first of which this call consumes.
        bool AcceptGenericClose ();
        void ExpectGenericClose ( std::string_view Where );
        // Shared tail for every FuncType spelling (`-> R`, `(A, B) -> R`,
        // bare `T -> R`): resolve the return type and wrap. The three
        // callers differ only in how `Params` was populated.
        [[nodiscard]] TypeId FinishFuncType ( TypeList Params, std::uint32_t Begin );

        // --- Grammar: JSX (ParseJsx.cpp) ---------------------------------

        [[nodiscard]] ExprId ParseJsxElement ();
        void ParseJsxChildren ( ExprList &Children, std::string_view CloseTag );
        [[nodiscard]] bool AtJsxStart () const;

        std::vector<Token> Tokens;
        std::size_t Pos = 0;
        // While parsing paren-less command-call arguments, a trailing `do`
        // must bind to the outer command call, not to the last argument
        // (Ruby/Crystal rule: `do` binds loosest, `{` binds tightest). Any
        // delimited context (parens, brackets, block bodies) clears it back.
        bool bNoDoBlock = false;
        AstContext &Context;
        Core::DiagEngine::Bag &Diagnostics;
        Core::StringInterner &Interner;
        std::string_view Source; // original file text, for JSX text runs
    };

} // namespace Frontend

} // namespace Volt
