#include "Volt/BackendCore/SymbolRegistry.hpp"

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Expr.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>
#include <variant>

namespace
{

std::mutex GSymbolMutex;
std::unordered_map<std::uint64_t, std::string> GSymbolTable;

} // namespace

std::string_view Volt::Backend::SymbolNameOf ( std::string_view Lexeme )
{
    return Lexeme.starts_with( ':' ) ? Lexeme.substr( 1 ) : Lexeme;
}

std::uint64_t Volt::Backend::SymbolValueOf ( std::string_view Name )
{
    // FNV-1a, 64-bit. Chosen for being fully specified by two constants — a
    // value that has to be reproduced identically by every target, and by a
    // build that reuses an archive emitted months earlier, cannot depend on a
    // library's version or a container's iteration order.
    constexpr std::uint64_t Offset = 14695981039346656037ULL;
    constexpr std::uint64_t Prime  = 1099511628211ULL;

    std::uint64_t Hash = Offset;
    for ( const char Ch : Name )
    {
        Hash ^= static_cast<std::uint64_t>( static_cast<unsigned char>( Ch ) );
        Hash *= Prime;
    }

    // Zero is reserved so a symbol is never indistinguishable from a
    // zero-initialised slot; the displacement is arbitrary and only has to be
    // stable.
    const std::uint64_t Value = Hash == 0 ? Prime : Hash;

    {
        std::lock_guard Lock( GSymbolMutex );
        GSymbolTable.try_emplace( Value, Name );
    }

    return Value;
}

extern "C" const char *_V_symbol_name ( std::uint64_t Value )
{
    std::lock_guard Lock( GSymbolMutex );
    if ( const auto It = GSymbolTable.find( Value ); It != GSymbolTable.end() )
    {
        return It->second.c_str();
    }
    return "";
}

std::vector<Volt::Backend::FSymbolEntry> Volt::Backend::CollectSymbols ( const BackendInput &Build, std::string &Clash )
{
    // Keyed by name so the walk order of the units cannot reach the output, and
    // so a name written in twenty files contributes one entry.
    std::map<std::string_view, std::uint64_t> Seen;

    for ( const UnitView &Unit : Build.Units )
    {
        if ( Unit.Ast == nullptr )
        {
            continue;
        }

        const std::size_t Count = Unit.Ast->ExprCount();
        for ( std::size_t Index = 0; Index < Count; ++Index )
        {
            const Frontend::ExprId Id{ static_cast<Frontend::ExprId::ValueType>( Index ) };
            const auto *Node = std::get_if<Frontend::SymbolLiteral>( &Unit.Ast->Expr( Id ) );
            if ( Node == nullptr )
            {
                continue;
            }

            const std::string_view Name = SymbolNameOf( Unit.Ast->Text( Node->Name ) );
            if ( Name.empty() )
            {
                continue;
            }
            Seen.emplace( Name, SymbolValueOf( Name ) );
        }
    }

    std::vector<FSymbolEntry> Table;
    Table.reserve( Seen.size() );
    for ( const auto &[Name, Value] : Seen )
    {
        Table.push_back( FSymbolEntry{ .Name = Name, .Value = Value } );
    }

    // A collision makes two source-level symbols one runtime identity, which is
    // a wrong program rather than a slow one. Detected against the sorted-by-
    // value order so it costs one pass and no second container.
    std::vector<FSymbolEntry> ByValue = Table;
    std::ranges::sort( ByValue, [] ( const FSymbolEntry &Lhs, const FSymbolEntry &Rhs ) { return Lhs.Value < Rhs.Value; } );
    for ( std::size_t Index = 1; Index < ByValue.size(); ++Index )
    {
        if ( ByValue[Index].Value == ByValue[Index - 1].Value )
        {
            Clash = std::string( ByValue[Index - 1].Name ) + "' and ':" + std::string( ByValue[Index].Name );
            return {};
        }
    }

    return Table;
}
