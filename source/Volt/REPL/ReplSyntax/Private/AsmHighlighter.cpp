// AsmHighlighter.cpp — a scanner for the disassembler's output.
//
// LLVM's MC printer emits AT&T syntax on x86 and a bare-register syntax
// elsewhere, so both are handled: a `%`-prefixed word is a register either way,
// and a bare word that names one is a register too. What it cannot know — an
// instruction set's full mnemonic list — it does not try to: the first word on
// a line is the mnemonic, which is true of every assembly syntax there is.

#include "Volt/ReplSyntax/Highlighter.hpp"

#include "SpanWriter.hpp"

#include <cstddef>
#include <string_view>
#include <unordered_set>

namespace
{

using Volt::Repl::Doc::EPaletteRole;

[[nodiscard]] bool IsWordChar ( const char C )
{
    return ( C >= 'a' and C <= 'z' ) or ( C >= 'A' and C <= 'Z' ) or ( C >= '0' and C <= '9' ) or C == '_' or C == '.' or
           C == '$' or C == '@';
}

[[nodiscard]] bool IsDigit ( const char C )
{
    return C >= '0' and C <= '9';
}

// Enough of x86-64 and AArch64 to catch a bare-register syntax. A name that is
// missed simply paints as text, which is why this list is allowed to be a
// list rather than a target description.
[[nodiscard]] bool IsRegisterName ( std::string_view Word )
{
    static const std::unordered_set<std::string_view> Words = {
        "rax",  "rbx",  "rcx",  "rdx",  "rsi",  "rdi",  "rbp",  "rsp",  "rip",  "eax",  "ebx",  "ecx",  "edx",
        "esi",  "edi",  "ebp",  "esp",  "ax",   "bx",   "cx",   "dx",   "si",   "di",   "bp",   "sp",   "al",
        "bl",   "cl",   "dl",   "ah",   "bh",   "ch",   "dh",   "sil",  "dil",  "bpl",  "spl",  "r8",   "r9",
        "r10",  "r11",  "r12",  "r13",  "r14",  "r15",  "r8d",  "r9d",  "r10d", "r11d", "r12d", "r13d", "r14d",
        "r15d", "r8w",  "r9w",  "r10w", "r11w", "r12w", "r13w", "r14w", "r15w", "r8b",  "r9b",  "r10b", "r11b",
        "r12b", "r13b", "r14b", "r15b", "sp",   "lr",   "pc",   "fp",   "wzr",  "xzr",  "wsp",
    };
    if ( Words.contains( Word ) )
    {
        return true;
    }

    // `xmm0`, `ymm15`, `zmm3`, `x0`, `w12`, `v8`, `s3`, `d1`, `q2`.
    static const std::unordered_set<std::string_view> Prefixes = { "xmm", "ymm", "zmm", "mm", "st" };
    for ( const std::string_view Prefix : Prefixes )
    {
        if ( Word.starts_with( Prefix ) and Word.size() > Prefix.size() )
        {
            return true;
        }
    }
    if ( Word.size() >= 2 and
         ( Word[0] == 'x' or Word[0] == 'w' or Word[0] == 'v' or Word[0] == 's' or Word[0] == 'd' or Word[0] == 'q' ) )
    {
        bool bDigits = true;
        for ( const char C : Word.substr( 1 ) )
        {
            bDigits = bDigits and IsDigit( C );
        }
        return bDigits;
    }
    return false;
}

} // namespace

Volt::Repl::Doc::Document Volt::Repl::Syntax::HighlightAsm ( const std::string_view Text, const Doc::Palette &Theme )
{
    SpanWriter Out( Theme );

    std::size_t Cursor  = 0;
    bool bLineHasOpcode = false; // has the first word of this line gone by?

    while ( Cursor < Text.size() )
    {
        const char C = Text[Cursor];

        if ( C == '\n' )
        {
            Out.Emit( Text.substr( Cursor, 1 ), EPaletteRole::Default );
            bLineHasOpcode = false;
            ++Cursor;
            continue;
        }

        if ( C == '#' or C == ';' )
        {
            const std::size_t Eol = Text.find( '\n', Cursor );
            const std::size_t End = Eol == std::string_view::npos ? Text.size() : Eol;
            Out.Emit( Text.substr( Cursor, End - Cursor ), EPaletteRole::Comment );
            Cursor = End;
            continue;
        }

        // `$42`, `$0x1000` — an AT&T immediate, sigil and all.
        if ( C == '$' )
        {
            std::size_t End = Cursor + 1;
            while ( End < Text.size() and IsWordChar( Text[End] ) )
            {
                ++End;
            }
            Out.Emit( Text.substr( Cursor, End - Cursor ), EPaletteRole::AsmImmediate );
            Cursor = End;
            continue;
        }

        if ( C == '%' )
        {
            std::size_t End = Cursor + 1;
            while ( End < Text.size() and IsWordChar( Text[End] ) )
            {
                ++End;
            }
            Out.Emit( Text.substr( Cursor, End - Cursor ), EPaletteRole::AsmRegister );
            Cursor = End;
            continue;
        }

        // `<_V_init_3+0x20>` — the symbolisation the printer puts after a
        // branch target.
        if ( C == '<' )
        {
            const std::size_t Close = Text.find( '>', Cursor );
            const std::size_t Eol   = Text.find( '\n', Cursor );
            if ( Close != std::string_view::npos and ( Eol == std::string_view::npos or Close < Eol ) )
            {
                Out.Emit( Text.substr( Cursor, Close + 1 - Cursor ), EPaletteRole::AsmLabel );
                Cursor = Close + 1;
                continue;
            }
        }

        if ( IsDigit( C ) )
        {
            std::size_t End = Cursor;
            while ( End < Text.size() and IsWordChar( Text[End] ) )
            {
                ++End;
            }
            Out.Emit( Text.substr( Cursor, End - Cursor ), EPaletteRole::AsmImmediate );
            Cursor = End;
            continue;
        }

        if ( IsWordChar( C ) )
        {
            std::size_t End = Cursor;
            while ( End < Text.size() and IsWordChar( Text[End] ) )
            {
                ++End;
            }
            const std::string_view Word = Text.substr( Cursor, End - Cursor );

            // A word followed by `:` labels the line rather than instructing
            // it, and does not consume the mnemonic slot.
            const bool bLabel = End < Text.size() and Text[End] == ':';

            EPaletteRole Role = EPaletteRole::Default;
            if ( bLabel )
            {
                Role = EPaletteRole::AsmLabel;
            }
            else if ( IsRegisterName( Word ) )
            {
                Role = EPaletteRole::AsmRegister;
            }
            else if ( not bLineHasOpcode )
            {
                Role           = EPaletteRole::AsmMnemonic;
                bLineHasOpcode = true;
            }

            Out.Emit( Word, Role );
            Cursor = End;
            continue;
        }

        if ( C == ' ' or C == '\t' )
        {
            std::size_t End = Cursor;
            while ( End < Text.size() and ( Text[End] == ' ' or Text[End] == '\t' ) )
            {
                ++End;
            }
            Out.Emit( Text.substr( Cursor, End - Cursor ), EPaletteRole::Default );
            Cursor = End;
            continue;
        }

        Out.Emit( Text.substr( Cursor, 1 ), EPaletteRole::Punctuation );
        ++Cursor;
    }

    return Out.Finish();
}
