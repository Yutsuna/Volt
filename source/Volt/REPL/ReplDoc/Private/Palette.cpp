// Palette.cpp — the two themes, and the role lookup.
//
// The hues are chosen the way a syntax theme is: literals warm, keywords
// saturated, structure quiet. What matters more than the exact triplets is
// that a category the eye needs to separate never shares one with its
// neighbour — a number and a string differ, a type and a function differ, and
// punctuation is dimmer than everything it separates.

#include "Volt/ReplDoc/Palette.hpp"

#include <array>
#include <cstddef>

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

Volt::Repl::Doc::Palette Volt::Repl::Doc::DefaultDarkPalette ()
{
    Palette Theme;

    Theme.Default       = Plain();
    Theme.Keyword       = Rgb( 197, 134, 192, EAttr::Bold ); // orchid
    Theme.Identifier    = Rgb( 212, 212, 212 );
    Theme.TypeName      = Rgb( 78, 201, 176 );  // teal
    Theme.FunctionName  = Rgb( 220, 220, 170 ); // pale gold
    Theme.Number        = Rgb( 181, 206, 168 ); // sage
    Theme.StringLiteral = Rgb( 206, 145, 120 ); // terracotta
    Theme.Symbol        = Rgb( 86, 156, 214 );  // azure
    Theme.BoolNil       = Rgb( 86, 156, 214 );
    Theme.Operator      = Rgb( 212, 212, 212 );
    Theme.Comment       = Rgb( 106, 153, 85, EAttr::Italic );
    Theme.Punctuation   = Rgb( 133, 133, 133 );

    Theme.ResultArrow     = Rgb( 106, 153, 85, EAttr::Bold );
    Theme.ResultValue     = Plain( EAttr::Bold );
    Theme.ResultType      = Rgb( 128, 128, 128, EAttr::Faint );
    Theme.InspectBrackets = Rgb( 78, 201, 176, EAttr::Faint );

    Theme.Prompt      = Rgb( 86, 156, 214, EAttr::Bold );
    Theme.GhostText   = Rgb( 100, 100, 100, EAttr::Faint );
    Theme.Error       = Rgb( 224, 108, 117, EAttr::Bold );
    Theme.Warning     = Rgb( 229, 192, 123 );
    Theme.PanelBorder = Rgb( 90, 90, 90 );
    Theme.PanelTitle  = Rgb( 86, 156, 214, EAttr::Bold );
    Theme.Selection   = Rgb( 86, 156, 214, EAttr::Reverse );

    Theme.IrRegister   = Rgb( 156, 220, 254 );
    Theme.IrGlobal     = Rgb( 220, 220, 170 );
    Theme.IrOpcode     = Rgb( 197, 134, 192, EAttr::Bold );
    Theme.IrType       = Rgb( 78, 201, 176 );
    Theme.AsmMnemonic  = Rgb( 197, 134, 192, EAttr::Bold );
    Theme.AsmRegister  = Rgb( 156, 220, 254 );
    Theme.AsmImmediate = Rgb( 181, 206, 168 );
    Theme.AsmLabel     = Rgb( 220, 220, 170 );

    return Theme;
}

Volt::Repl::Doc::Palette Volt::Repl::Doc::DefaultLightPalette ()
{
    Palette Theme;

    Theme.Default       = Plain();
    Theme.Keyword       = Rgb( 128, 0, 128, EAttr::Bold );
    Theme.Identifier    = Rgb( 36, 41, 46 );
    Theme.TypeName      = Rgb( 0, 92, 197 );
    Theme.FunctionName  = Rgb( 111, 66, 193 );
    Theme.Number        = Rgb( 0, 92, 197 );
    Theme.StringLiteral = Rgb( 3, 47, 98 );
    Theme.Symbol        = Rgb( 0, 92, 197 );
    Theme.BoolNil       = Rgb( 0, 92, 197 );
    Theme.Operator      = Rgb( 36, 41, 46 );
    Theme.Comment       = Rgb( 106, 115, 125, EAttr::Italic );
    Theme.Punctuation   = Rgb( 106, 115, 125 );

    Theme.ResultArrow     = Rgb( 34, 134, 58, EAttr::Bold );
    Theme.ResultValue     = Plain( EAttr::Bold );
    Theme.ResultType      = Rgb( 106, 115, 125, EAttr::Faint );
    Theme.InspectBrackets = Rgb( 0, 92, 197, EAttr::Faint );

    Theme.Prompt      = Rgb( 0, 92, 197, EAttr::Bold );
    Theme.GhostText   = Rgb( 170, 170, 170, EAttr::Faint );
    Theme.Error       = Rgb( 203, 36, 49, EAttr::Bold );
    Theme.Warning     = Rgb( 176, 136, 0 );
    Theme.PanelBorder = Rgb( 175, 184, 193 );
    Theme.PanelTitle  = Rgb( 0, 92, 197, EAttr::Bold );
    Theme.Selection   = Rgb( 0, 92, 197, EAttr::Reverse );

    Theme.IrRegister   = Rgb( 0, 92, 197 );
    Theme.IrGlobal     = Rgb( 111, 66, 193 );
    Theme.IrOpcode     = Rgb( 128, 0, 128, EAttr::Bold );
    Theme.IrType       = Rgb( 0, 92, 197 );
    Theme.AsmMnemonic  = Rgb( 128, 0, 128, EAttr::Bold );
    Theme.AsmRegister  = Rgb( 0, 92, 197 );
    Theme.AsmImmediate = Rgb( 34, 134, 58 );
    Theme.AsmLabel     = Rgb( 111, 66, 193 );

    return Theme;
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
