// Palette.cpp — the themes, and the role lookup.
//
// Three of them: Catppuccin Mocha for a dark terminal, Catppuccin Latte for a
// light one, and a monochrome fallback that keeps only the attributes. The
// first two are a designed pair rather than a taste — a role keeps its meaning
// across both, and the contrast was decided by people who measured it.
//
// What the *assignment* has to preserve is separation: a category the eye
// needs to tell apart never shares a swatch with its neighbour. A number and a
// string differ, a type and a function differ, and punctuation sits on an
// overlay rather than on a hue so that it recedes behind everything it
// separates.

#include "Volt/ReplDoc/Palette.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

Volt::Repl::Doc::Color Volt::Repl::Doc::RoleColor ( const Palette &Theme, const EPaletteRole Role )
{
    // One Color per role, in declaration order, asserted in the header. Reading
    // the structure through a span of its own member type is what makes a
    // role a subscript rather than a thirty-arm switch nobody would keep
    // current.
    const auto Index = static_cast<std::size_t>( Role );
    if ( Index >= static_cast<std::size_t>( EPaletteRole::Count ) )
    {
        return Theme.Default;
    }

    const auto *First = &Theme.Default;
    return First[Index]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

// --- Catppuccin ---------------------------------------------------------------
//
// Mocha for a dark terminal, Latte for a light one. Not a taste: the two are
// designed as a pair, so a role keeps its *meaning* across them — a keyword is
// mauve in both, a string is green in both — and the whole palette moves
// together when the background does. Rolling our own would have meant
// re-deciding two dozen hues twice and getting the contrast wrong once.
//
// The role assignments follow Catppuccin's own syntax-highlighting guidance,
// which is why `Number` and `BoolNil` share peach (both are constants) and why
// comments sit on an overlay rather than on a hue.

namespace
{

using Volt::Repl::Doc::Rgb;

// One flavour's swatch. A struct rather than a namespace so the role mapping
// below can be written once and instantiated for each.
struct Mocha
{

    static constexpr auto Flamingo = Rgb( 0xF2, 0xCD, 0xCD );
    static constexpr auto Mauve    = Rgb( 0xCB, 0xA6, 0xF7 );
    static constexpr auto Red      = Rgb( 0xF3, 0x8B, 0xA8 );
    static constexpr auto Peach    = Rgb( 0xFA, 0xB3, 0x87 );
    static constexpr auto Yellow   = Rgb( 0xF9, 0xE2, 0xAF );
    static constexpr auto Green    = Rgb( 0xA6, 0xE3, 0xA1 );
    static constexpr auto Teal     = Rgb( 0x94, 0xE2, 0xD5 );
    static constexpr auto Sky      = Rgb( 0x89, 0xDC, 0xEB );
    static constexpr auto Sapphire = Rgb( 0x74, 0xC7, 0xEC );
    static constexpr auto Blue     = Rgb( 0x89, 0xB4, 0xFA );
    static constexpr auto Text     = Rgb( 0xCD, 0xD6, 0xF4 );
    static constexpr auto Overlay2 = Rgb( 0x93, 0x99, 0xB2 );
    static constexpr auto Overlay1 = Rgb( 0x7F, 0x84, 0x9C );
    static constexpr auto Overlay0 = Rgb( 0x6C, 0x70, 0x86 );
    static constexpr auto Surface2 = Rgb( 0x58, 0x5B, 0x70 );
};

struct Latte
{

    static constexpr auto Flamingo = Rgb( 0xDD, 0x78, 0x78 );
    static constexpr auto Mauve    = Rgb( 0x88, 0x39, 0xEF );
    static constexpr auto Red      = Rgb( 0xD2, 0x0F, 0x39 );
    static constexpr auto Peach    = Rgb( 0xFE, 0x64, 0x0B );
    static constexpr auto Yellow   = Rgb( 0xDF, 0x8E, 0x1D );
    static constexpr auto Green    = Rgb( 0x40, 0xA0, 0x2B );
    static constexpr auto Teal     = Rgb( 0x17, 0x92, 0x99 );
    static constexpr auto Sky      = Rgb( 0x04, 0xA5, 0xE5 );
    static constexpr auto Sapphire = Rgb( 0x20, 0x9F, 0xB5 );
    static constexpr auto Blue     = Rgb( 0x1E, 0x66, 0xF5 );
    static constexpr auto Text     = Rgb( 0x4C, 0x4F, 0x69 );
    static constexpr auto Overlay2 = Rgb( 0x7C, 0x7F, 0x93 );
    static constexpr auto Overlay1 = Rgb( 0x8C, 0x8F, 0xA1 );
    static constexpr auto Overlay0 = Rgb( 0x9C, 0xA0, 0xB0 );
    static constexpr auto Surface2 = Rgb( 0xAC, 0xB0, 0xBE );
};

// The two flavours differ only in their swatch, so the assignment of roles to
// swatch names is written once and instantiated twice. A role that gains a hue
// here gains it in both, which is the property that makes them a pair.
template <typename Flavour> [[nodiscard]] Volt::Repl::Doc::Palette Catppuccin ()
{
    using namespace Volt::Repl::Doc;

    Palette Theme;

    Theme.Default       = Flavour::Text;
    Theme.Keyword       = Flavour::Mauve;
    Theme.Identifier    = Flavour::Text;
    Theme.TypeName      = Flavour::Yellow;
    Theme.FunctionName  = Flavour::Blue;
    Theme.Number        = Flavour::Peach;
    Theme.StringLiteral = Flavour::Green;
    Theme.Symbol        = Flavour::Flamingo;
    Theme.BoolNil       = Flavour::Peach;
    Theme.Operator      = Flavour::Sky;
    Theme.Comment       = Flavour::Overlay0;
    Theme.Punctuation   = Flavour::Overlay2;

    Theme.Keyword.Attribute = EAttr::Bold;
    Theme.Comment.Attribute = EAttr::Italic;

    Theme.ResultArrow     = Flavour::Teal;
    Theme.ResultValue     = Flavour::Text;
    Theme.ResultType      = Flavour::Overlay1;
    Theme.InspectBrackets = Flavour::Yellow;

    Theme.ResultArrow.Attribute     = EAttr::Bold;
    Theme.ResultValue.Attribute     = EAttr::Bold;
    Theme.ResultType.Attribute      = EAttr::Faint;
    Theme.InspectBrackets.Attribute = EAttr::Faint;

    Theme.Prompt      = Flavour::Blue;
    Theme.GhostText   = Flavour::Surface2;
    Theme.Error       = Flavour::Red;
    Theme.Warning     = Flavour::Yellow;
    Theme.PanelBorder = Flavour::Surface2;
    Theme.PanelTitle  = Flavour::Mauve;
    Theme.Selection   = Flavour::Blue;

    Theme.Prompt.Attribute     = EAttr::Bold;
    Theme.GhostText.Attribute  = EAttr::Faint;
    Theme.Error.Attribute      = EAttr::Bold;
    Theme.PanelTitle.Attribute = EAttr::Bold;
    Theme.Selection.Attribute  = EAttr::Reverse;

    Theme.IrRegister   = Flavour::Sapphire;
    Theme.IrGlobal     = Flavour::Blue;
    Theme.IrOpcode     = Flavour::Mauve;
    Theme.IrType       = Flavour::Yellow;
    Theme.AsmMnemonic  = Flavour::Mauve;
    Theme.AsmRegister  = Flavour::Sapphire;
    Theme.AsmImmediate = Flavour::Peach;
    Theme.AsmLabel     = Flavour::Blue;

    Theme.IrOpcode.Attribute    = EAttr::Bold;
    Theme.AsmMnemonic.Attribute = EAttr::Bold;

    return Theme;
}

} // namespace

Volt::Repl::Doc::Palette Volt::Repl::Doc::DefaultDarkPalette ()
{
    return Catppuccin<Mocha>();
}

Volt::Repl::Doc::Palette Volt::Repl::Doc::DefaultLightPalette ()
{
    return Catppuccin<Latte>();
}

Volt::Repl::Doc::Palette Volt::Repl::Doc::MonochromePalette ()
{
    Palette Theme;

    // Every field left at its default — no colour — and then the handful of
    // roles whose *meaning* is a weight rather than a hue put back. A REPL on
    // a sixteen-colour terminal still wants a type suffix to recede and an
    // error to stand out.
    Theme.Keyword         = Plain( EAttr::Bold );
    Theme.Comment         = Plain( EAttr::Faint );
    Theme.ResultArrow     = Plain( EAttr::Bold );
    Theme.ResultValue     = Plain( EAttr::Bold );
    Theme.ResultType      = Plain( EAttr::Faint );
    Theme.InspectBrackets = Plain( EAttr::Faint );
    Theme.Prompt          = Plain( EAttr::Bold );
    Theme.GhostText       = Plain( EAttr::Faint );
    Theme.Error           = Plain( EAttr::Bold );
    Theme.PanelBorder     = Plain( EAttr::Faint );
    Theme.PanelTitle      = Plain( EAttr::Bold );
    Theme.Selection       = Plain( EAttr::Reverse );
    Theme.IrOpcode        = Plain( EAttr::Bold );
    Theme.AsmMnemonic     = Plain( EAttr::Bold );

    return Theme;
}

std::string_view Volt::Repl::Doc::RoleName ( const EPaletteRole Role )
{
    switch ( Role )
    {
#define VOLT_ROLE( Name )                                                                                                        \
    case EPaletteRole::Name:                                                                                                     \
        return #Name;

        VOLT_ROLE( Default )
        VOLT_ROLE( Keyword )
        VOLT_ROLE( Identifier )
        VOLT_ROLE( TypeName )
        VOLT_ROLE( FunctionName )
        VOLT_ROLE( Number )
        VOLT_ROLE( StringLiteral )
        VOLT_ROLE( Symbol )
        VOLT_ROLE( BoolNil )
        VOLT_ROLE( Operator )
        VOLT_ROLE( Comment )
        VOLT_ROLE( Punctuation )
        VOLT_ROLE( ResultArrow )
        VOLT_ROLE( ResultValue )
        VOLT_ROLE( ResultType )
        VOLT_ROLE( InspectBrackets )
        VOLT_ROLE( Prompt )
        VOLT_ROLE( GhostText )
        VOLT_ROLE( Error )
        VOLT_ROLE( Warning )
        VOLT_ROLE( PanelBorder )
        VOLT_ROLE( PanelTitle )
        VOLT_ROLE( Selection )
        VOLT_ROLE( IrRegister )
        VOLT_ROLE( IrGlobal )
        VOLT_ROLE( IrOpcode )
        VOLT_ROLE( IrType )
        VOLT_ROLE( AsmMnemonic )
        VOLT_ROLE( AsmRegister )
        VOLT_ROLE( AsmImmediate )
        VOLT_ROLE( AsmLabel )

#undef VOLT_ROLE
    case EPaletteRole::Count:
        break;
    }
    return "?";
}

Volt::Repl::Doc::Palette Volt::Repl::Doc::DistinctPalette ()
{
    Palette Theme;

    // Written through the same flat view `RoleColor` reads through — the
    // assert in the header is what makes that legal — so a role added to the
    // enum gets a distinct colour here with no edit at all.
    Color *First = &Theme.Default;
    for ( std::size_t Index = 0; Index < static_cast<std::size_t>( EPaletteRole::Count ); ++Index )
    {
        // The index, spread across two channels so a palette of more than 255
        // roles would still be injective.
        First[Index] = Rgb( static_cast<std::uint8_t>( Index & 0xFFU ), // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                            static_cast<std::uint8_t>( ( Index >> 8U ) & 0xFFU ), 0 );
    }
    return Theme;
}

Volt::Repl::Doc::EPaletteRole Volt::Repl::Doc::RoleOfColor ( const Color Style )
{
    if ( Style.bDefault )
    {
        return EPaletteRole::Count;
    }

    const auto Index = static_cast<std::size_t>( Style.R ) | ( static_cast<std::size_t>( Style.G ) << 8U );
    if ( Style.B != 0 or Index >= static_cast<std::size_t>( EPaletteRole::Count ) )
    {
        return EPaletteRole::Count;
    }
    return static_cast<EPaletteRole>( Index );
}

std::span<const std::string_view> Volt::Repl::Doc::PaletteNames ()
{
    static constexpr std::array<std::string_view, 4> Names = { "auto", "dark", "light", "mono" };
    return Names;
}

std::optional<Volt::Repl::Doc::Palette> Volt::Repl::Doc::PaletteByName ( const std::string_view Name, const bool bDark )
{
    if ( Name == "auto" )
    {
        return bDark ? DefaultDarkPalette() : DefaultLightPalette();
    }
    if ( Name == "dark" )
    {
        return DefaultDarkPalette();
    }
    if ( Name == "light" )
    {
        return DefaultLightPalette();
    }
    // "none" is what NO_COLOR-shaped tooling tends to spell it, and it means
    // the same thing here: attributes, no hues.
    if ( Name == "mono" or Name == "none" )
    {
        return MonochromePalette();
    }
    return std::nullopt;
}
