#pragma once

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
    class Parser
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
            while ( Check( TokenKind::Newline ) || Check( TokenKind::Semicolon ) )
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
        [[nodiscard]] ExprId ParseSubExpression ( std::string_view Text, Core::SourceRange Range );
        [[nodiscard]] ExprId ParseCommandCallArgs ( ExprId Callee, Core::SourceRange Start );
        void ParseCallArguments ( ExprList &Args, SymbolList &ArgNames, TokenKind Close );
        [[nodiscard]] bool CanStartCommandArgument () const;

        // --- Grammar: statements (ParseStmt.cpp) -------------------------

        [[nodiscard]] StmtId ParseStatement ();
        [[nodiscard]] StmtId ParseIf ();
        [[nodiscard]] StmtId ParseElsif ();
        [[nodiscard]] StmtId ParseWhile ();
        [[nodiscard]] StmtId ParseReturn ();
        [[nodiscard]] StmtId ParseExprOrLocalStatement ();
        void ParseStatementBlock ( StmtList &Out );
        [[nodiscard]] StmtId ApplyModifiers ( StmtId Inner );

        // --- Grammar: declarations (ParseDecl.cpp) -----------------------

        [[nodiscard]] bool AtDeclaration () const;
        [[nodiscard]] DeclId ParseDeclaration ();
        [[nodiscard]] DeclId ParseModule ();
        [[nodiscard]] DeclId ParseClass ();
        [[nodiscard]] DeclId ParseStruct ();
        [[nodiscard]] DeclId ParseMixin ();
        [[nodiscard]] DeclId ParseMethod ( bool bAbstract );
        [[nodiscard]] DeclId ParseInclude ();
        [[nodiscard]] DeclId ParseComponent ();
        [[nodiscard]] DeclId ParseCircuit ();
        [[nodiscard]] DeclId ParseAnnotation ();
        [[nodiscard]] DeclId ParseFieldOrMember ();
        void ParseDeclBlock ( DeclList &Out );
        void ParseParameterList ( ParamList &Out );
        [[nodiscard]] SymbolList ParseGenericParams ();

        // --- Grammar: types (ParseType.cpp) ------------------------------

        [[nodiscard]] TypeId ParseType ();
        [[nodiscard]] TypeId ParseTypePrimary ();
        [[nodiscard]] bool AtTypeStart () const;

        // --- Grammar: JSX (ParseJsx.cpp) ---------------------------------

        [[nodiscard]] ExprId ParseJsxElement ();
        void ParseJsxChildren ( ExprList &Children, std::string_view CloseTag );
        [[nodiscard]] bool AtJsxStart () const;

        std::vector<Token> Tokens;
        std::size_t Pos = 0;
        AstContext &Context;
        Core::DiagEngine::Bag &Diagnostics;
        Core::StringInterner &Interner;
        std::string_view Source; // original file text, for JSX text runs
    };

} // namespace Frontend

} // namespace Volt
