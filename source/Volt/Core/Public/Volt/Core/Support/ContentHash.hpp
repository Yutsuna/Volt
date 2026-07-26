#pragma once

#include "Core_export.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Core
{

    // Non-cryptographic 64-bit content hash for cache keys (stdlib frontend
    // cache, native artifact cache — see .agents/PROGRESS-issue-61.md). Not a
    // security primitive: collision resistance against an adversary is not a
    // requirement, only change-sensitivity across ordinary source edits.
    // FNV-1a is self-contained on purpose — pulling in LLVM's xxhash/MD5 for
    // this would make `Core`, the base module every other module depends on,
    // depend on all of LLVM just to key a cache.
    inline constexpr std::uint64_t FnvOffsetBasis = 0xcbf29ce484222325ULL;
    inline constexpr std::uint64_t FnvPrime       = 0x100000001b3ULL;

    [[nodiscard]] constexpr std::uint64_t HashBytes ( std::string_view Bytes, std::uint64_t Seed = FnvOffsetBasis ) noexcept
    {
        std::uint64_t State = Seed;
        for ( const char Byte : Bytes )
        {
            State ^= static_cast<std::uint8_t>( Byte );
            State *= FnvPrime;
        }
        return State;
    }

    // Folds another hash value into Seed — for combining several already-hashed
    // fields (path hash + content hash, or key + target triple + opt level)
    // without re-hashing their raw bytes.
    [[nodiscard]] constexpr std::uint64_t CombineHash ( std::uint64_t Seed, std::uint64_t Value ) noexcept
    {
        return HashBytes( std::string_view{ reinterpret_cast<const char *>( &Value ), sizeof( Value ) }, Seed );
    }

    [[nodiscard]] inline std::uint64_t CombineHash ( std::uint64_t Seed, std::string_view Text ) noexcept
    {
        return HashBytes( Text, Seed );
    }

    // Reads Path whole and folds its bytes into Seed. std::nullopt on any
    // read failure (missing/unreadable file) — callers treat that as "cannot
    // form a key", not a crash.
    [[nodiscard]] CORE_EXPORT std::optional<std::uint64_t> HashFile ( const std::filesystem::path &Path,
                                                                      std::uint64_t Seed = FnvOffsetBasis );

    // Hashes an already-sorted list of files by path *and* content, folding
    // each in turn into Seed. Callers own the sort (Driver's stdlib walk
    // already sorts by path for TypeBinder ordering reasons — reuse that
    // order rather than re-deriving it here).
    [[nodiscard]] CORE_EXPORT std::optional<std::uint64_t> HashFileTree ( const std::vector<std::filesystem::path> &SortedFiles,
                                                                          std::uint64_t Seed = FnvOffsetBasis );

    [[nodiscard]] CORE_EXPORT std::string ToHex ( std::uint64_t Value );

} // namespace Core

} // namespace Volt
