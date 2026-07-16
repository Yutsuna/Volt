#pragma once

#include "Volt/Core/Diagnostics/SourceLocation.hpp"
#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/StringInterner.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Node.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/AST/Type.hpp"

#include <utility>
#include <vector>

namespace Volt
{

    namespace Frontend
    {

        /// All AST storage for a single source file: five value-arenas plus the
        /// list of top-level items. One per file keeps parsing and later sema
        /// fully local and parallelisable. The string interner is shared and
        /// outlives every AstContext.
        class AstContext
        {

        public:

            AstContext( Core::StringInterner& InInterner, Core::FileId InFile ) : Interner( &InInterner ), File( InFile )
            {
            }

            [[nodiscard]] Core::StringInterner& Strings() const
            {
                return *Interner;
            }

            [[nodiscard]] Core::FileId FileId() const
            {
                return File;
            }

            [[nodiscard]] std::string_view Text( Symbol Handle ) const
            {
                return Interner->Resolve( Handle );
            }

            // --- Node creation (overloaded on the category variant) ----------

            [[nodiscard]] ExprId Add( ExprNode Node )
            {
                return Exprs.Add( std::move( Node ) );
            }

            [[nodiscard]] StmtId Add( StmtNode Node )
            {
                return Stmts.Add( std::move( Node ) );
            }

            [[nodiscard]] DeclId Add( DeclNode Node )
            {
                return Decls.Add( std::move( Node ) );
            }

            [[nodiscard]] TypeId Add( TypeNode Node )
            {
                return Types.Add( std::move( Node ) );
            }

            [[nodiscard]] ParamId Add( Param Node )
            {
                return Params.Add( std::move( Node ) );
            }

            // --- Node access -------------------------------------------------

            [[nodiscard]] ExprNode& Expr( ExprId Id )
            {
                return Exprs.Get( Id );
            }
            [[nodiscard]] const ExprNode& Expr( ExprId Id ) const
            {
                return Exprs.Get( Id );
            }

            [[nodiscard]] StmtNode& Stmt( StmtId Id )
            {
                return Stmts.Get( Id );
            }
            [[nodiscard]] const StmtNode& Stmt( StmtId Id ) const
            {
                return Stmts.Get( Id );
            }

            [[nodiscard]] DeclNode& Decl( DeclId Id )
            {
                return Decls.Get( Id );
            }
            [[nodiscard]] const DeclNode& Decl( DeclId Id ) const
            {
                return Decls.Get( Id );
            }

            [[nodiscard]] TypeNode& Type( TypeId Id )
            {
                return Types.Get( Id );
            }
            [[nodiscard]] const TypeNode& Type( TypeId Id ) const
            {
                return Types.Get( Id );
            }

            [[nodiscard]] Param& GetParam( ParamId Id )
            {
                return Params.Get( Id );
            }
            [[nodiscard]] const Param& GetParam( ParamId Id ) const
            {
                return Params.Get( Id );
            }

            /// Number of expression slots. Lets a pass iterate every node by
            /// index — new nodes it appends land past the original count.
            [[nodiscard]] std::size_t ExprCount() const
            {
                return Exprs.Size();
            }

            // --- Top-level items ---------------------------------------------

            std::vector<DeclId> TopDecls;
            std::vector<StmtId> TopStmts;

        private:

            Core::StringInterner* Interner = nullptr;
            Core::FileId          File;

            Core::Arena<ExprNode, ExprId> Exprs;
            Core::Arena<StmtNode, StmtId> Stmts;
            Core::Arena<DeclNode, DeclId> Decls;
            Core::Arena<TypeNode, TypeId> Types;
            Core::Arena<Param, ParamId>   Params;
        };

    }

}
