#pragma once

// Serialize.hpp — a generic binary Serialize<T>/Deserialize<T> pair driven by
// Meta::ForEachField (Reflect.hpp). Any aggregate (an ExprNode/StmtNode/
// DeclNode/TypeNode alternative, Sema::PassStats, ...) round-trips with zero
// per-type code; only the handful of non-aggregate leaves below need a
// hand-written overload. This is the mechanism rules/meta-first.md asks for:
// a hand-rolled per-node serializer would be exactly the duplicate traversal
// the architecture forbids.
//
// Core::Arena<T,IdType> and Core::StringInterner are *not* handled here even
// though they are leaves of the same kind — both need replay-in-order rather
// than a bare read/write (an Arena's Ids are its insertion order; a Symbol
// *is* the interner's insertion order), so they get their own named
// SerializeArena/DeserializeArena and SerializeInterner/DeserializeInterner
// entry points instead of an ambient Serialize(Writer&, const Arena<T,Id>&)
// overload that would silently do the wrong thing if ever called on a
// non-fresh arena.

#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Core/Support/StringInterner.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace Volt
{

namespace Meta
{

    /// Append-only byte sink. No structure of its own — every Serialize
    /// overload below appends its own framing (lengths, discriminants).
    class Writer
    {

    public:

        void Bytes ( const void *Data, std::size_t Size )
        {
            const auto *Begin = static_cast<const std::byte *>( Data );
            Buffer.insert( Buffer.end(), Begin, Begin + Size );
        }

        [[nodiscard]] const std::vector<std::byte> &Data () const
        {
            return Buffer;
        }

    private:

        std::vector<std::byte> Buffer;
    };

    /// Sequential byte source over a buffer it does not own. Every read past
    /// the end sets a sticky failure flag instead of throwing or reading out
    /// of bounds — a truncated/corrupt cache file is always a reported miss,
    /// never a crash (the design's on-disk-layout contract).
    class Reader
    {

    public:

        explicit Reader ( std::span<const std::byte> InData ) : Bytes( InData )
        {
        }

        [[nodiscard]] bool Read ( void *Out, std::size_t Size )
        {
            if ( bFailed or Offset + Size > Bytes.size() )
            {
                bFailed = true;
                return false;
            }
            std::memcpy( Out, Bytes.data() + Offset, Size );
            Offset += Size;
            return true;
        }

        [[nodiscard]] bool Failed () const
        {
            return bFailed;
        }

    private:

        std::span<const std::byte> Bytes;
        std::size_t Offset = 0;
        bool bFailed       = false;
    };

    // --- Forward declarations ---------------------------------------------
    //
    // Every overload is declared here first so a nested call (a struct field
    // that is itself a variant of aggregates) resolves by ordinary lookup
    // regardless of definition order below.

    template <typename T>
        requires( std::is_arithmetic_v<T> or std::is_enum_v<T> )
    void Serialize ( Writer &W, const T &Value );
    template <typename T>
        requires( std::is_arithmetic_v<T> or std::is_enum_v<T> )
    [[nodiscard]] bool Deserialize ( Reader &R, T &Value );

    template <typename Tag> void Serialize ( Writer &W, const Core::TypedId<Tag> &Id );
    template <typename Tag> [[nodiscard]] bool Deserialize ( Reader &R, Core::TypedId<Tag> &Id );

    template <typename T, std::size_t N> void Serialize ( Writer &W, const Core::SmallVec<T, N> &Vec );
    template <typename T, std::size_t N> [[nodiscard]] bool Deserialize ( Reader &R, Core::SmallVec<T, N> &Vec );

    void Serialize ( Writer &W, const std::string &Value );
    [[nodiscard]] bool Deserialize ( Reader &R, std::string &Value );

    template <typename T> void Serialize ( Writer &W, const std::vector<T> &Vec );
    template <typename T> [[nodiscard]] bool Deserialize ( Reader &R, std::vector<T> &Vec );

    // std::vector<bool> is a bit-packed specialisation with no real T&
    // references, so the generic vector<T> loop below (which binds
    // `const T &Element`) cannot instantiate over it — a dedicated overload,
    // one bool per byte, no different in spirit from any other leaf.
    void Serialize ( Writer &W, const std::vector<bool> &Vec );
    [[nodiscard]] bool Deserialize ( Reader &R, std::vector<bool> &Vec );

    template <typename T> void Serialize ( Writer &W, const std::optional<T> &Opt );
    template <typename T> [[nodiscard]] bool Deserialize ( Reader &R, std::optional<T> &Opt );

    template <typename... Alts> void Serialize ( Writer &W, const std::variant<Alts...> &Var );
    template <typename... Alts> [[nodiscard]] bool Deserialize ( Reader &R, std::variant<Alts...> &Var );

    template <typename K, typename V, typename... Rest>
    void Serialize ( Writer &W, const std::unordered_map<K, V, Rest...> &Map );
    template <typename K, typename V, typename... Rest>
    [[nodiscard]] bool Deserialize ( Reader &R, std::unordered_map<K, V, Rest...> &Map );

    template <Reflected T> void Serialize ( Writer &W, const T &Object );
    template <Reflected T> [[nodiscard]] bool Deserialize ( Reader &R, T &Object );

    // --- Leaves --------------------------------------------------------

    template <typename T>
        requires( std::is_arithmetic_v<T> or std::is_enum_v<T> )
    void Serialize ( Writer &W, const T &Value )
    {
        W.Bytes( &Value, sizeof( T ) );
    }

    template <typename T>
        requires( std::is_arithmetic_v<T> or std::is_enum_v<T> )
    [[nodiscard]] bool Deserialize ( Reader &R, T &Value )
    {
        return R.Read( &Value, sizeof( T ) );
    }

    // Core::TypedId<Tag>: not an aggregate (it declares constructors), so it
    // needs its own overload rather than falling through to ForEachField.
    // Reconstructed via the explicit-value constructor, InvalidValue included
    // — Provenance (VOLT_CHECKED_IDS builds only) is per-process bookkeeping,
    // never persisted.
    template <typename Tag> void Serialize ( Writer &W, const Core::TypedId<Tag> &Id )
    {
        Serialize( W, Id.Value );
    }

    template <typename Tag> [[nodiscard]] bool Deserialize ( Reader &R, Core::TypedId<Tag> &Id )
    {
        typename Core::TypedId<Tag>::ValueType Value{};
        if ( not Deserialize( R, Value ) )
        {
            return false;
        }
        Id = Core::TypedId<Tag>( Value );
        return true;
    }

    // Core::SmallVec<T,N>: also not an aggregate.
    template <typename T, std::size_t N> void Serialize ( Writer &W, const Core::SmallVec<T, N> &Vec )
    {
        const auto Count = static_cast<std::uint32_t>( Vec.Size() );
        Serialize( W, Count );
        for ( const T &Element : Vec )
        {
            Serialize( W, Element );
        }
    }

    template <typename T, std::size_t N> [[nodiscard]] bool Deserialize ( Reader &R, Core::SmallVec<T, N> &Vec )
    {
        std::uint32_t Count = 0;
        if ( not Deserialize( R, Count ) )
        {
            return false;
        }
        Vec = Core::SmallVec<T, N>{};
        Vec.Reserve( Count );
        for ( std::uint32_t Index = 0; Index < Count; ++Index )
        {
            T Element{};
            if ( not Deserialize( R, Element ) )
            {
                return false;
            }
            Vec.PushBack( std::move( Element ) );
        }
        return true;
    }

    inline void Serialize ( Writer &W, const std::string &Value )
    {
        const auto Length = static_cast<std::uint32_t>( Value.size() );
        Serialize( W, Length );
        if ( Length != 0 )
        {
            W.Bytes( Value.data(), Length );
        }
    }

    [[nodiscard]] inline bool Deserialize ( Reader &R, std::string &Value )
    {
        std::uint32_t Length = 0;
        if ( not Deserialize( R, Length ) )
        {
            return false;
        }
        Value.resize( Length );
        return Length == 0 or R.Read( Value.data(), Length );
    }

    template <typename T> void Serialize ( Writer &W, const std::vector<T> &Vec )
    {
        const auto Count = static_cast<std::uint32_t>( Vec.size() );
        Serialize( W, Count );
        for ( const T &Element : Vec )
        {
            Serialize( W, Element );
        }
    }

    template <typename T> [[nodiscard]] bool Deserialize ( Reader &R, std::vector<T> &Vec )
    {
        std::uint32_t Count = 0;
        if ( not Deserialize( R, Count ) )
        {
            return false;
        }
        Vec.clear();
        Vec.reserve( Count );
        for ( std::uint32_t Index = 0; Index < Count; ++Index )
        {
            T Element{};
            if ( not Deserialize( R, Element ) )
            {
                return false;
            }
            Vec.push_back( std::move( Element ) );
        }
        return true;
    }

    inline void Serialize ( Writer &W, const std::vector<bool> &Vec )
    {
        const auto Count = static_cast<std::uint32_t>( Vec.size() );
        Serialize( W, Count );
        for ( const bool Element : Vec )
        {
            Serialize( W, Element );
        }
    }

    [[nodiscard]] inline bool Deserialize ( Reader &R, std::vector<bool> &Vec )
    {
        std::uint32_t Count = 0;
        if ( not Deserialize( R, Count ) )
        {
            return false;
        }
        Vec.clear();
        Vec.reserve( Count );
        for ( std::uint32_t Index = 0; Index < Count; ++Index )
        {
            bool Element = false;
            if ( not Deserialize( R, Element ) )
            {
                return false;
            }
            Vec.push_back( Element );
        }
        return true;
    }

    template <typename T> void Serialize ( Writer &W, const std::optional<T> &Opt )
    {
        const bool bHas = Opt.has_value();
        Serialize( W, bHas );
        if ( bHas )
        {
            Serialize( W, *Opt );
        }
    }

    template <typename T> [[nodiscard]] bool Deserialize ( Reader &R, std::optional<T> &Opt )
    {
        bool bHas = false;
        if ( not Deserialize( R, bHas ) )
        {
            return false;
        }
        if ( not bHas )
        {
            Opt.reset();
            return true;
        }
        T Value{};
        if ( not Deserialize( R, Value ) )
        {
            return false;
        }
        Opt = std::move( Value );
        return true;
    }

    template <typename... Alts> void Serialize ( Writer &W, const std::variant<Alts...> &Var )
    {
        Serialize( W, static_cast<std::uint32_t>( Var.index() ) );
        std::visit(
            [&W] ( const auto &Alt )
            {
                using Bare = std::remove_cvref_t<decltype( Alt )>;
                if constexpr ( not std::same_as<Bare, std::monostate> )
                {
                    Serialize( W, Alt );
                }
            },
            Var );
    }

    namespace Detail
    {

        // One alternative of a Deserialize<variant> dispatch: constructs the
        // Index'th alternative in place from the reader (monostate needs no
        // bytes at all).
        template <std::size_t Index, typename Variant> [[nodiscard]] bool DeserializeAlt ( Reader &R, Variant &Var )
        {
            using Alt = std::variant_alternative_t<Index, Variant>;
            if constexpr ( std::same_as<Alt, std::monostate> )
            {
                Var = Alt{};
                return true;
            }
            else
            {
                Alt Value{};
                if ( not Deserialize( R, Value ) )
                {
                    return false;
                }
                Var = std::move( Value );
                return true;
            }
        }

    } // namespace Detail

    template <typename... Alts> [[nodiscard]] bool Deserialize ( Reader &R, std::variant<Alts...> &Var )
    {
        std::uint32_t Index = 0;
        if ( not Deserialize( R, Index ) )
        {
            return false;
        }
        using VariantType = std::variant<Alts...>;
        if ( Index >= std::variant_size_v<VariantType> )
        {
            return false;
        }
        bool bOk = false;
        [&]<std::size_t... Is>( std::index_sequence<Is...> )
        {
            ( ( Is == Index ? ( bOk = Detail::DeserializeAlt<Is>( R, Var ), true ) : false ) or ... );
        }( std::index_sequence_for<Alts...>{} );
        return bOk;
    }

    template <typename K, typename V, typename... Rest> void Serialize ( Writer &W, const std::unordered_map<K, V, Rest...> &Map )
    {
        const auto Count = static_cast<std::uint32_t>( Map.size() );
        Serialize( W, Count );
        for ( const auto &[Key, Value] : Map )
        {
            Serialize( W, Key );
            Serialize( W, Value );
        }
    }

    template <typename K, typename V, typename... Rest>
    [[nodiscard]] bool Deserialize ( Reader &R, std::unordered_map<K, V, Rest...> &Map )
    {
        std::uint32_t Count = 0;
        if ( not Deserialize( R, Count ) )
        {
            return false;
        }
        Map.clear();
        Map.reserve( Count );
        for ( std::uint32_t Index = 0; Index < Count; ++Index )
        {
            K Key{};
            V Value{};
            if ( not Deserialize( R, Key ) or not Deserialize( R, Value ) )
            {
                return false;
            }
            Map.emplace( std::move( Key ), std::move( Value ) );
        }
        return true;
    }

    // --- The generic case: any reflected aggregate --------------------

    template <Reflected T> void Serialize ( Writer &W, const T &Object )
    {
        ForEachField( Object, [&W] ( std::string_view, const auto &Field ) { Serialize( W, Field ); } );
    }

    template <Reflected T> [[nodiscard]] bool Deserialize ( Reader &R, T &Object )
    {
        bool bOk = true;
        ForEachField( Object,
                      [&R, &bOk] ( std::string_view, auto &Field )
                      {
                          if ( bOk )
                          {
                              bOk = Deserialize( R, Field );
                          }
                      } );
        return bOk;
    }

    // --- Arena / StringInterner: replay-in-order, not a bare leaf ------

    /// Serialises an Arena in insertion order. Deserialize replays Add() in
    /// the same order, so the Ids it hands out are identical to the
    /// original's with no remap table (ast-value.md: Arena is a std::vector).
    template <typename T, typename IdType> void SerializeArena ( Writer &W, const Core::Arena<T, IdType> &A )
    {
        const auto Count = static_cast<std::uint32_t>( A.Size() );
        Serialize( W, Count );
        for ( const T &Element : A )
        {
            Serialize( W, Element );
        }
    }

    /// A *fresh* Arena is expected: this replays Add() from empty and does
    /// not clear existing contents first.
    template <typename T, typename IdType> [[nodiscard]] bool DeserializeArena ( Reader &R, Core::Arena<T, IdType> &A )
    {
        std::uint32_t Count = 0;
        if ( not Deserialize( R, Count ) )
        {
            return false;
        }
        A.Reserve( Count );
        for ( std::uint32_t Index = 0; Index < Count; ++Index )
        {
            T Element{};
            if ( not Deserialize( R, Element ) )
            {
                return false;
            }
            static_cast<void>( A.Add( std::move( Element ) ) );
        }
        return true;
    }

    /// Serialises every interned string except slot 0 (the interner's
    /// constructor always reserves it for the empty string), so replay can
    /// start a fresh interner and reproduce identical Symbol values.
    inline void SerializeInterner ( Writer &W, const Core::StringInterner &Interner )
    {
        const auto Count = static_cast<std::uint32_t>( Interner.Size() - 1 );
        Serialize( W, Count );
        for ( std::uint32_t Index = 1; Index <= Count; ++Index )
        {
            const std::string_view Text = Interner.Resolve( Core::Symbol{ Index } );
            Serialize( W, std::string{ Text } );
        }
    }

    /// A *fresh* StringInterner is expected (only its constructor-reserved
    /// empty-string slot present) — replaying Intern() in original order onto
    /// anything else would desynchronise every Symbol after this call.
    [[nodiscard]] inline bool DeserializeInterner ( Reader &R, Core::StringInterner &Interner )
    {
        std::uint32_t Count = 0;
        if ( not Deserialize( R, Count ) )
        {
            return false;
        }
        for ( std::uint32_t Index = 0; Index < Count; ++Index )
        {
            std::string Text;
            if ( not Deserialize( R, Text ) )
            {
                return false;
            }
            static_cast<void>( Interner.Intern( Text ) );
        }
        return true;
    }

} // namespace Meta

} // namespace Volt
