#include "Volt/Frontend/Lexer/Token.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <unordered_map>

namespace Volt
{
namespace Frontend
{

    namespace
    {

        constexpr std::size_t KindCount = static_cast<std::size_t>( TokenKind::Count );

        constexpr std::array<std::string_view, KindCount> BuildNames ()
        {
            std::array<std::string_view, KindCount> Names{};
            std::size_t Index = 0;
#define VOLT_TOKEN( Name ) Names[Index++] = #Name;
#define VOLT_PUNCT( Name, Spelling ) Names[Index++] = #Name;
#define VOLT_KEYWORD( Name, Spelling ) Names[Index++] = #Name;
#include "Volt/Frontend/Lexer/TokenKind.inl"
            return Names;
        }

        constexpr std::array<std::string_view, KindCount> BuildSpellings ()
        {
            std::array<std::string_view, KindCount> Spellings{};
            std::size_t Index = 0;
#define VOLT_TOKEN( Name ) Spellings[Index++] = "";
#define VOLT_PUNCT( Name, Spelling ) Spellings[Index++] = Spelling;
#define VOLT_KEYWORD( Name, Spelling ) Spellings[Index++] = Spelling;
#include "Volt/Frontend/Lexer/TokenKind.inl"
            return Spellings;
        }

        const std::unordered_map<std::string_view, TokenKind> &KeywordTable ()
        {
            static const std::unordered_map<std::string_view, TokenKind> Table = {
#define VOLT_KEYWORD( Name, Spelling ) { Spelling, TokenKind::Name },
#include "Volt/Frontend/Lexer/TokenKind.inl"
            };
            return Table;
        }

    } // namespace

} // namespace Frontend

} // namespace Volt

std::string_view Volt::Frontend::TokenName ( TokenKind Kind )
{
    static constexpr auto Names = BuildNames();
    const auto Index            = static_cast<std::size_t>( Kind );
    return Index < Names.size() ? Names[Index] : std::string_view{ "?" };
}

std::string_view Volt::Frontend::TokenSpelling ( TokenKind Kind )
{
    static constexpr auto Spellings = BuildSpellings();
    const auto Index                = static_cast<std::size_t>( Kind );
    return Index < Spellings.size() ? Spellings[Index] : std::string_view{};
}

Volt::Frontend::TokenKind Volt::Frontend::KeywordLookup ( std::string_view Text )
{
    const auto &Table = KeywordTable();
    if ( const auto It = Table.find( Text ); It != Table.end() )
    {
        return It->second;
    }
    return TokenKind::Identifier;
}
