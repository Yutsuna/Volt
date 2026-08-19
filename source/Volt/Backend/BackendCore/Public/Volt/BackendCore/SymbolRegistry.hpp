#pragma once

// SymbolRegistry.hpp — the runtime identity behind a Volt `:symbol`.
//
// A `Core::Symbol` is a *spelling* handle interned per file (rules/ast-value.md:
// "not interned symbols, since symbols are per-file"), and every CompileUnit
// owns its own StringInterner. A Volt Symbol is the opposite of that: an
// identity, and `:pending == :pending` has to hold whichever two files wrote
// the two halves. Emitting the frontend handle made it hold only inside one
// file — two units each got their own integer for the same name, and the
// comparison silently answered false.
//
// So the runtime value is minted here, from the *text*, and it is a hash rather
// than a table ordinal for one reason: the stdlib's object code is cached across
// builds (issue #61), so any numbering that depends on what else the build
// contains would move under an archive that was already emitted. A pure
// function of the name cannot.
//
// Shared by all three targets for the same reason Mangler is: a symbol emitted
// by one must be recognisable to a reader of any other.

#include "BackendCore_export.hpp"
#include "Volt/BackendCore/BackendInput.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Volt
{

namespace Backend
{

    // The name a symbol lexeme spells. The lexer makes the token at the colon
    // (Lexer::LexSymbolOrColon), so the interned text keeps it and a name never
    // does — the same strip ExprInferencer applies for `.responds_to? :name`.
    [[nodiscard]] BACKENDCORE_EXPORT std::string_view SymbolNameOf ( std::string_view Lexeme );

    // The value `:Name` evaluates to, in every unit of every build. FNV-1a over
    // the name — 64 bits because that is the width the type claiming
    // `SymbolLiteral` declares, and nothing here picks it.
    [[nodiscard]] BACKENDCORE_EXPORT std::uint64_t SymbolValueOf ( std::string_view Name );

    struct FSymbolEntry
    {

        // Points into the declaring unit's interner, which outlives codegen.
        std::string_view Name;
        std::uint64_t Value = 0;
    };

    // Every distinct `:symbol` written anywhere in the build, ordered by name so
    // two runs of the same build emit byte-identical tables.
    //
    // Read off the ASTs rather than accumulated while bodies are emitted: a
    // precompiled stdlib unit is never *defined* in the module being built
    // (LlvmLifecycle::EmitUnit returns early for it) but its names still have to
    // be in the table, and a generic body that is never instantiated must not
    // put its names there twice.
    //
    // Two distinct names sharing one value would make them `==` at runtime.
    // It cannot be repaired here — the value is the identity — so it is
    // reported: on collision this returns an empty vector and writes the two
    // names into `Clash`.
    [[nodiscard]] BACKENDCORE_EXPORT std::vector<FSymbolEntry> CollectSymbols ( const BackendInput &Build, std::string &Clash );

} // namespace Backend

} // namespace Volt
