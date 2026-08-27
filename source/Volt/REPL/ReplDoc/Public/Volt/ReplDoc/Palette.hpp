#pragma once

// Palette.hpp — every colour the REPL can use, named by what it means.
//
// One structure, one field per semantic role, and nothing anywhere else in the
// tree writes an escape sequence of its own. That is what makes a theme a
// value rather than a refactor: `ReplSyntax` asks for `Keyword`, `ReplQuery`
// asks for `PanelTitle`, and only `ReplTui` knows either one is ever going to
// become `\x1b[38;2;…m`.
//
// The roles are deliberately finer than a terminal's sixteen colours. A REPL
// that paints every literal the same is a REPL where a stray quote looks like
// a number, and the whole point of colouring input as it is typed is that the
// eye catches what the parser would.

#include "ReplDoc_export.hpp"

#include <cstdint>
#include <string_view>

namespace Volt
{

namespace Repl
{

    namespace Doc
    {

        // Text attributes, as a bitmask: a role is a colour *and* a weight, and
        // `Faint` in particular carries meaning on its own — it is what ghost
        // text and a type suffix are, whatever hue the theme gives them.
        enum class EAttr : std::uint8_t
        {

            None      = 0,
            Bold      = 1U << 0U,
            Faint     = 1U << 1U,
            Italic    = 1U << 2U,
            Underline = 1U << 3U,
            Reverse   = 1U << 4U,
        };

        [[nodiscard]] constexpr EAttr operator|( EAttr Lhs, EAttr Rhs )
        {
            return static_cast<EAttr>( static_cast<std::uint8_t>( Lhs ) | static_cast<std::uint8_t>( Rhs ) );
        }

        [[nodiscard]] constexpr bool HasAttr ( EAttr Set, EAttr Which )
        {
            return ( static_cast<std::uint8_t>( Set ) & static_cast<std::uint8_t>( Which ) ) != 0;
        }

        // One colour: a 24-bit triplet plus attributes.
        //
        // `bDefault` is not "black" — it is *no colour at all*, so a span
        // carrying it inherits whatever the terminal is already painting with.
        // A REPL that forced a foreground on ordinary text would fight the
        // user's own scheme on every character it printed.
        struct Color
        {

            std::uint8_t R  = 0;
            std::uint8_t G  = 0;
            std::uint8_t B  = 0;
            bool bDefault   = true;
            EAttr Attribute = EAttr::None;

            [[nodiscard]] constexpr bool operator==( const Color & ) const = default;
        };

        [[nodiscard]] constexpr Color Rgb ( std::uint8_t R, std::uint8_t G, std::uint8_t B, EAttr Attribute = EAttr::None )
        {
            return Color{ .R = R, .G = G, .B = B, .bDefault = false, .Attribute = Attribute };
        }

        [[nodiscard]] constexpr Color Plain ( EAttr Attribute = EAttr::None )
        {
            return Color{ .R = 0, .G = 0, .B = 0, .bDefault = true, .Attribute = Attribute };
        }

        // What a highlighter names when it asks for a colour. The enum, rather
        // than a pointer-to-member, so the token-kind mapping tables are plain
        // static arrays a reader can check by eye.
        enum class EPaletteRole : std::uint8_t
        {

            // --- Volt tokens, in input and in output alike -------------------
            Default = 0,
            Keyword,
            Identifier,
            TypeName,
            FunctionName,
            Number,
            StringLiteral,
            Symbol,
            BoolNil,
            Operator,
            Comment,
            Punctuation,

            // --- The result line ---------------------------------------------
            ResultArrow,
            ResultValue,
            ResultType,
            InspectBrackets,

            // --- Interface ----------------------------------------------------
            Prompt,
            GhostText,
            Error,
            Warning,
            PanelBorder,
            PanelTitle,
            Selection,

            // --- LLVM IR and machine code ------------------------------------
            IrRegister,
            IrGlobal,
            IrOpcode,
            IrType,
            AsmMnemonic,
            AsmRegister,
            AsmImmediate,
            AsmLabel,

            Count,
        };

        // A flat structure, one field per role, in the order the enum declares
        // them — `RoleColor` indexes it, and the static assert below is what
        // keeps the two from drifting.
        struct Palette
        {

            Color Default;
            Color Keyword;
            Color Identifier;
            Color TypeName;
            Color FunctionName;
            Color Number;
            Color StringLiteral;
            Color Symbol;
            Color BoolNil;
            Color Operator;
            Color Comment;
            Color Punctuation;

            Color ResultArrow;
            Color ResultValue;
            Color ResultType;
            Color InspectBrackets;

            Color Prompt;
            Color GhostText;
            Color Error;
            Color Warning;
            Color PanelBorder;
            Color PanelTitle;
            Color Selection;

            Color IrRegister;
            Color IrGlobal;
            Color IrOpcode;
            Color IrType;
            Color AsmMnemonic;
            Color AsmRegister;
            Color AsmImmediate;
            Color AsmLabel;
        };

        static_assert( sizeof( Palette ) == sizeof( Color ) * static_cast<std::size_t>( EPaletteRole::Count ),
                       "Palette must hold exactly one Color per EPaletteRole, in declaration order" );

        // The colour a role resolves to. Reads the structure as the array it
        // is laid out as, which is legal precisely because every member has the
        // same type and the assert above pins the count.
        [[nodiscard]] REPLDOC_EXPORT Color RoleColor ( const Palette &Theme, EPaletteRole Role );

        // The role's own name — `"Keyword"`, `"IrOpcode"`. For diagnostics, and
        // for a test that wants to read a rendered document back as the roles
        // that produced it.
        [[nodiscard]] REPLDOC_EXPORT std::string_view RoleName ( EPaletteRole Role );

        // A palette in which every role has a colour of its own, and no two
        // share one.
        //
        // Not a theme — the colours are indices, and nobody would want to look
        // at it. What it is for is the inverse of `RoleColor`: given a rendered
        // span, `RoleOfColor` says which role produced it, and that only works
        // when the mapping is injective. A golden test of a highlighter is
        // exactly that question.
        [[nodiscard]] REPLDOC_EXPORT Palette DistinctPalette ();

        // Which role a colour came from, under `DistinctPalette`. `Count` for
        // a colour that palette never produces.
        [[nodiscard]] REPLDOC_EXPORT EPaletteRole RoleOfColor ( Color Style );

        [[nodiscard]] REPLDOC_EXPORT Palette DefaultDarkPalette ();
        [[nodiscard]] REPLDOC_EXPORT Palette DefaultLightPalette ();

        // Every colour dropped, every attribute kept.
        //
        // Not the same thing as "do not colour": a pipe gets no escape at all
        // (ReplTui simply never emits one), while a terminal that answers only
        // sixteen colours still wants bold and faint to mean what they mean.
        [[nodiscard]] REPLDOC_EXPORT Palette MonochromePalette ();

    } // namespace Doc

} // namespace Repl

} // namespace Volt
