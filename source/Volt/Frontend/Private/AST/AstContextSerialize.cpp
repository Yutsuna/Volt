// AstContextSerialize.cpp — AstContext::SerializeCache/DeserializeCache
// (Issue #61, Phase 2c). Every ExprNode/StmtNode/DeclNode/TypeNode
// alternative is a Meta::Reflected aggregate already — the same guarantee
// AstPrinter/ForEachField rely on — so this is nothing but calls to the
// generic SerializeArena/Serialize primitives (rules/meta-first.md).

#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"

namespace Volt
{

namespace Frontend
{

    void AstContext::SerializeCache ( Meta::Writer &W ) const
    {
        Meta::SerializeArena( W, Exprs );
        Meta::SerializeArena( W, Stmts );
        Meta::SerializeArena( W, Decls );
        Meta::SerializeArena( W, Types );
        Meta::SerializeArena( W, Params );
        Meta::Serialize( W, TopDecls );
        Meta::Serialize( W, TopStmts );

        const auto Counter = static_cast<std::uint64_t>( SymCounter );
        Meta::Serialize( W, Counter );
    }

    bool AstContext::DeserializeCache ( Meta::Reader &R )
    {
        if ( not Meta::DeserializeArena( R, Exprs ) )
        {
            return false;
        }
        if ( not Meta::DeserializeArena( R, Stmts ) )
        {
            return false;
        }
        if ( not Meta::DeserializeArena( R, Decls ) )
        {
            return false;
        }
        if ( not Meta::DeserializeArena( R, Types ) )
        {
            return false;
        }
        if ( not Meta::DeserializeArena( R, Params ) )
        {
            return false;
        }
        if ( not Meta::Deserialize( R, TopDecls ) )
        {
            return false;
        }
        if ( not Meta::Deserialize( R, TopStmts ) )
        {
            return false;
        }

        std::uint64_t Counter = 0;
        if ( not Meta::Deserialize( R, Counter ) )
        {
            return false;
        }
        SymCounter = static_cast<std::size_t>( Counter );
        return true;
    }

} // namespace Frontend

} // namespace Volt
