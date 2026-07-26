#pragma once

#include "Frontend_export.hpp"
#include "Volt/Core/Diagnostics/SourceLocation.hpp"
#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/AST/Type.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Volt
{

namespace Meta
{
    class Writer;
    class Reader;
} // namespace Meta

namespace Frontend
{

    /// All AST storage for a single source file: five value-arenas plus the
    /// list of top-level items. One per file keeps parsing and later sema
    /// fully local and parallelisable. The string interner is shared and
    /// outlives every AstContext.
    class FRONTEND_EXPORT AstContext
    {

    public:

        AstContext ( Core::StringInterner &InInterner, Core::FileId InFile ) : Interner( &InInterner ), File( InFile )
        {
        }

        [[nodiscard]] Core::StringInterner &Strings () const
        {
            return *Interner;
        }

        [[nodiscard]] Core::FileId FileId () const
        {
            return File;
        }

        [[nodiscard]] std::string_view Text ( Symbol Handle ) const
        {
            return Interner->Resolve( Handle );
        }

        [[nodiscard]] Symbol MakeUniqueSymbol ( std::string_view Prefix )
        {
            const std::string Name = std::string( Prefix ) + "_" + std::to_string( SymCounter++ );
            return Interner->Intern( Name );
        }

        // --- Node creation (overloaded on the category variant) ----------

        [[nodiscard]] ExprId Add ( ExprNode Node )
        {
            return Exprs.Add( std::move( Node ) );
        }

        [[nodiscard]] StmtId Add ( StmtNode Node )
        {
            return Stmts.Add( std::move( Node ) );
        }

        [[nodiscard]] DeclId Add ( DeclNode Node )
        {
            return Decls.Add( std::move( Node ) );
        }

        [[nodiscard]] TypeId Add ( TypeNode Node )
        {
            return Types.Add( std::move( Node ) );
        }

        [[nodiscard]] ParamId Add ( Param Node )
        {
            return Params.Add( std::move( Node ) );
        }

        // --- Node access -------------------------------------------------

        [[nodiscard]] ExprNode &Expr ( ExprId Id )
        {
            return Exprs.Get( Id );
        }
        [[nodiscard]] const ExprNode &Expr ( ExprId Id ) const
        {
            return Exprs.Get( Id );
        }

        [[nodiscard]] StmtNode &Stmt ( StmtId Id )
        {
            return Stmts.Get( Id );
        }
        [[nodiscard]] const StmtNode &Stmt ( StmtId Id ) const
        {
            return Stmts.Get( Id );
        }

        [[nodiscard]] DeclNode &Decl ( DeclId Id )
        {
            return Decls.Get( Id );
        }
        [[nodiscard]] const DeclNode &Decl ( DeclId Id ) const
        {
            return Decls.Get( Id );
        }

        [[nodiscard]] TypeNode &Type ( TypeId Id )
        {
            return Types.Get( Id );
        }
        [[nodiscard]] const TypeNode &Type ( TypeId Id ) const
        {
            return Types.Get( Id );
        }

        [[nodiscard]] Param &GetParam ( ParamId Id )
        {
            return Params.Get( Id );
        }
        [[nodiscard]] const Param &GetParam ( ParamId Id ) const
        {
            return Params.Get( Id );
        }

        /// Number of expression slots. Lets a pass iterate every node by
        /// index — new nodes it appends land past the original count.
        [[nodiscard]] std::size_t ExprCount () const
        {
            return Exprs.Size();
        }

        /// Number of declaration slots, for the same by-index pass iteration.
        [[nodiscard]] std::size_t DeclCount () const
        {
            return Decls.Size();
        }

        /// The remaining slot counts, for a caller that needs to bracket the
        /// nodes one nested parse appended (see Parser::ParseSubExpression).
        [[nodiscard]] std::size_t StmtCount () const
        {
            return Stmts.Size();
        }

        [[nodiscard]] std::size_t TypeCount () const
        {
            return Types.Size();
        }

        [[nodiscard]] std::size_t ParamCount () const
        {
            return Params.Size();
        }

        // --- Top-level items ---------------------------------------------

        std::vector<DeclId> TopDecls;
        std::vector<StmtId> TopStmts;

        // --- Frontend cache (Issue #61) -----------------------------------
        //
        // File/Interner are not covered here: File is a per-run SourceManager
        // handle the Driver already re-derives on every invocation (cache hit
        // or not — it re-registers the stdlib's file text regardless, since
        // that costs nothing next to lexing/parsing/sema), and Interner is a
        // reference this AstContext never owns. Both are supplied through the
        // real constructor, same as on a fresh (non-cached) unit; only the
        // five arenas plus TopDecls/TopStmts/the unique-symbol counter round-
        // trip here. Every ExprNode/StmtNode/DeclNode/TypeNode alternative is
        // a Meta::Reflected aggregate already (the same guarantee the AST
        // printer/walker rely on), so SerializeArena/DeserializeArena reach
        // all of them with zero per-node code.
        void SerializeCache ( Meta::Writer &W ) const;

        // Expects a freshly-constructed AstContext (same Interner/File
        // contract as the ordinary constructor establishes) with all five
        // arenas still empty.
        [[nodiscard]] bool DeserializeCache ( Meta::Reader &R );

    private:

        Core::StringInterner *Interner = nullptr;
        Core::FileId File;

        Core::Arena<ExprNode, ExprId> Exprs;
        Core::Arena<StmtNode, StmtId> Stmts;
        Core::Arena<DeclNode, DeclId> Decls;
        Core::Arena<TypeNode, TypeId> Types;
        Core::Arena<Param, ParamId> Params;

        std::size_t SymCounter = 0;
    };

} // namespace Frontend

} // namespace Volt
