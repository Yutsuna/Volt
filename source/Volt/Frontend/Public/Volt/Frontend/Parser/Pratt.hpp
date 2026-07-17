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

    /// Every prefix operator that lowers to a Unary node binds here.
    inline constexpr int PrefixBindingPower = 100;

    /// Binding power for an infix operator (assignments included), generated from Pratt.inl.
    [[nodiscard]] constexpr BindingPower InfixBinding ( TokenKind Kind )
    {
        switch ( Kind )
        {
#define VOLT_INFIX( Name, Lbp, RAssoc )                                                                                                                                  \
    case TokenKind::Name:                                                                                                                                                \
        return BindingPower{ Lbp, ( RAssoc ) ? ( Lbp ) : ( Lbp + 1 ) };
#define VOLT_ASSIGN( Name, Lbp, RAssoc ) VOLT_INFIX( Name, Lbp, RAssoc )
#include "Volt/Frontend/Parser/Pratt.inl"
        default:
            return BindingPower{ 0, 0 };
        }
    }

    [[nodiscard]] constexpr bool IsInfixOperator ( TokenKind Kind )
    {
        return InfixBinding( Kind ).Left != 0;
    }

    /// True for the VOLT_ASSIGN rows of Pratt.inl (`=`, `+=`, ...): infix
    /// operators that build an Assign node instead of a Binary one.
    [[nodiscard]] constexpr bool IsAssignment ( TokenKind Kind )
    {
        switch ( Kind )
        {
#define VOLT_ASSIGN( Name, Lbp, RAssoc )                                                                                                                                 \
    case TokenKind::Name:                                                                                                                                                \
        return true;
#include "Volt/Frontend/Parser/Pratt.inl"
        default:
            return false;
        }
    }

    /// True for the VOLT_PREFIX rows of Pratt.inl: prefix operators that
    /// build a Unary node at PrefixBindingPower.
    [[nodiscard]] constexpr bool IsPrefixOperator ( TokenKind Kind )
    {
        switch ( Kind )
        {
#define VOLT_PREFIX( Name )                                                                                                                                              \
    case TokenKind::Name:                                                                                                                                                \
        return true;
#include "Volt/Frontend/Parser/Pratt.inl"
        default:
            return false;
        }
    }

} // namespace Frontend

} // namespace Volt
