#pragma once

#include "Volt/Frontend/Lexer/Token.hpp"

namespace Volt
{

    namespace Frontend
    {

        struct BindingPower
        {

            int Left  = 0; // 0 means "not an infix operator"
            int Right = 0;
        };

        /// Binding power for an infix operator, generated from Pratt.inl.
        [[nodiscard]] constexpr BindingPower InfixBinding( TokenKind Kind )
        {
            switch ( Kind )
            {
#define VOLT_INFIX( Name, Lbp, RAssoc )                                                                                          \
    case TokenKind::Name:                                                                                                        \
        return BindingPower{ Lbp, ( RAssoc ) ? ( Lbp ) : ( Lbp + 1 ) };
#include "Volt/Frontend/Parser/Pratt.inl"
                default:
                    return BindingPower{ 0, 0 };
            }
        }

        [[nodiscard]] constexpr bool IsInfixOperator( TokenKind Kind )
        {
            return InfixBinding( Kind ).Left != 0;
        }

    }

}
