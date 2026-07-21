// TypeChecker.cpp — Order 30 pass: checks literal and structure types against TypeStore.

#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Sema/Pass.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace Volt
{

namespace Sema
{

    void TypeChecker ( PassContext &Context )
    {
        const std::size_t Count = Context.Ast.ExprCount();
        for ( std::size_t Index = 0; Index < Count; ++Index )
        {
            const Frontend::ExprId Id{ static_cast<std::uint32_t>( Index ) };
            if ( !Id.IsValid() )
            {
                continue;
            }

            const Frontend::ExprNode &Node = Context.Ast.Expr( Id );

            std::string_view LiteralKindName{};
            if ( std::holds_alternative<Frontend::IntLiteral>( Node ) )
            {
                LiteralKindName = "IntLiteral";
            }
            else if ( std::holds_alternative<Frontend::FloatLiteral>( Node ) )
            {
                LiteralKindName = "FloatLiteral";
            }
            else if ( std::holds_alternative<Frontend::BoolLiteral>( Node ) )
            {
                LiteralKindName = "BoolLiteral";
            }
            else if ( std::holds_alternative<Frontend::StringLiteral>( Node ) )
            {
                LiteralKindName = "StringLiteral";
            }
            else if ( std::holds_alternative<Frontend::SymbolLiteral>( Node ) )
            {
                LiteralKindName = "SymbolLiteral";
            }
            else if ( std::holds_alternative<Frontend::ArrayLit>( Node ) )
            {
                LiteralKindName = "ArrayLiteral";
            }
            else if ( std::holds_alternative<Frontend::HashLit>( Node ) )
            {
                LiteralKindName = "HashLiteral";
            }

            if ( !LiteralKindName.empty() )
            {
                const Symbol LitSym  = Context.Ast.Strings().Intern( LiteralKindName );
                const auto OptLayout = Context.Types.LookupLiteral( LitSym );
                if ( !OptLayout )
                {
                    Context.Diags.Report( Core::Diagnostic{ Core::ESeverity::Warning,
                                                            Core::SourceRange{},
                                                            "unbound literal type: " + std::string{ LiteralKindName },
                                                            {} } );
                }
            }
        }
    }

} // namespace Sema

} // namespace Volt
