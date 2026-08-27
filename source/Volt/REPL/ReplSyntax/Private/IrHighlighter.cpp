// IrHighlighter.cpp — a scanner for LLVM IR, in the hundred lines it deserves.
//
// Not a parser and not trying to be. LLVM IR is regular enough at the token
// level that four sigils carry most of the meaning — `%` a local, `@` a global,
// `!` metadata, `;` a comment — and the rest is a word list. An opcode nobody
// listed paints as plain text, which is the right failure for a highlighter.

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
           C == '$' or C == '-';
}

[[nodiscard]] bool IsDigit ( const char C )
{
    return C >= '0' and C <= '9';
}

// The instructions and the module-level keywords, which is what the eye scans
// an IR dump for. Types are recognised by shape (`i32`, `<4 x float>`) rather
// than listed, since the integer widths are open-ended.
[[nodiscard]] bool IsOpcode ( const std::string_view Word )
{
    static const std::unordered_set<std::string_view> Words = {
        "add",
        "fadd",
        "sub",
        "fsub",
        "mul",
        "fmul",
        "udiv",
        "sdiv",
        "fdiv",
        "urem",
        "srem",
        "frem",
        "shl",
        "lshr",
        "ashr",
        "and",
        "or",
        "xor",
        "alloca",
        "load",
        "store",
        "fence",
        "cmpxchg",
        "atomicrmw",
        "getelementptr",
        "trunc",
        "zext",
        "sext",
        "fptrunc",
        "fpext",
        "fptoui",
        "fptosi",
        "uitofp",
        "sitofp",
        "ptrtoint",
        "inttoptr",
        "bitcast",
        "addrspacecast",
        "icmp",
        "fcmp",
        "phi",
        "select",
        "call",
        "va_arg",
        "landingpad",
        "catchpad",
        "cleanuppad",
        "ret",
        "br",
        "switch",
        "indirectbr",
        "invoke",
        "callbr",
        "resume",
        "catchswitch",
        "catchret",
        "cleanupret",
        "unreachable",
        "extractelement",
        "insertelement",
        "shufflevector",
        "extractvalue",
        "insertvalue",
        "freeze",
        "define",
        "declare",
        "global",
        "constant",
        "type",
        "target",
        "source_filename",
        "attributes",
        "module",
        "asm",
        "tail",
        "musttail",
        "notail",
        "to",
        "nsw",
        "nuw",
        "exact",
        "inbounds",
        "volatile",
        "align",
        "unnamed_addr",
        "local_unnamed_addr",
        "private",
        "internal",
        "external",
        "linkonce",
        "linkonce_odr",
        "weak",
        "weak_odr",
        "common",
        "appending",
        "available_externally",
        "dso_local",
        "dso_preemptable",
        "hidden",
        "protected",
        "default",
        "thread_local",
    };
    return Words.contains( Word );
}

// `i32`, `i1`, `i128` — an `i` and nothing but digits after it. Plus the named
// types every function signature carries.
[[nodiscard]] bool IsTypeWord ( const std::string_view Word )
{
    if ( Word.size() > 1 and Word.front() == 'i' )
    {
        bool bAllDigits = true;
        for ( const char C : Word.substr( 1 ) )
        {
            bAllDigits = bAllDigits and IsDigit( C );
        }
        if ( bAllDigits )
        {
            return true;
        }
    }

    static const std::unordered_set<std::string_view> Words = {
        "void",     "ptr",       "half",  "bfloat", "float",    "double", "fp128",
        "x86_fp80", "ppc_fp128", "label", "token",  "metadata", "opaque",
    };
    return Words.contains( Word );
}

// A sigil-prefixed name: `%0`, `%"quoted name"`, `@main`, `!dbg`.
[[nodiscard]] std::size_t SigilRun ( const std::string_view Text, const std::size_t Start )
{
    std::size_t Cursor = Start + 1;
    if ( Cursor < Text.size() and Text[Cursor] == '"' )
    {
        ++Cursor;
        while ( Cursor < Text.size() and Text[Cursor] != '"' )
        {
            ++Cursor;
        }
        return Cursor < Text.size() ? Cursor + 1 : Cursor;
    }
    while ( Cursor < Text.size() and IsWordChar( Text[Cursor] ) )
    {
        ++Cursor;
    }
    return Cursor;
}

} // namespace

Volt::Repl::Doc::Document Volt::Repl::Syntax::HighlightIr ( const std::string_view Text, const Doc::Palette &Theme )
{
    SpanWriter Out( Theme );

    std::size_t Cursor = 0;
    while ( Cursor < Text.size() )
    {
        const char C = Text[Cursor];

        if ( C == ';' )
        {
            const std::size_t Eol = Text.find( '\n', Cursor );
            const std::size_t End = Eol == std::string_view::npos ? Text.size() : Eol;
            Out.Emit( Text.substr( Cursor, End - Cursor ), EPaletteRole::Comment );
            Cursor = End;
            continue;
        }

        if ( C == '%' or C == '@' or C == '!' )
        {
            const std::size_t End = SigilRun( Text, Cursor );
            const EPaletteRole Role =
                C == '%' ? EPaletteRole::IrRegister : ( C == '@' ? EPaletteRole::IrGlobal : EPaletteRole::Symbol );
            Out.Emit( Text.substr( Cursor, End - Cursor ), Role );
            Cursor = End;
            continue;
        }

        if ( C == '"' )
        {
            std::size_t End = Cursor + 1;
            while ( End < Text.size() and Text[End] != '"' )
            {
                End += Text[End] == '\\' and End + 1 < Text.size() ? std::size_t{ 2 } : std::size_t{ 1 };
            }
            End = End < Text.size() ? End + 1 : End;
            Out.Emit( Text.substr( Cursor, End - Cursor ), EPaletteRole::StringLiteral );
            Cursor = End;
            continue;
        }

        if ( IsDigit( C ) or ( C == '-' and Cursor + 1 < Text.size() and IsDigit( Text[Cursor + 1] ) ) )
        {
            std::size_t End = Cursor + 1;
            while ( End < Text.size() and ( IsWordChar( Text[End] ) or Text[End] == 'x' ) )
            {
                ++End;
            }
            Out.Emit( Text.substr( Cursor, End - Cursor ), EPaletteRole::Number );
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

            // `entry:` — a basic block's label, which is the one thing in an
            // IR dump the eye scans for and which no word list could name.
            const bool bAtLineStart = Cursor == 0 or Text[Cursor - 1] == '\n';
            if ( bAtLineStart and End < Text.size() and Text[End] == ':' )
            {
                Out.Emit( Word, EPaletteRole::IrGlobal );
                Cursor = End;
                continue;
            }

            EPaletteRole Role = EPaletteRole::Default;
            if ( IsTypeWord( Word ) )
            {
                Role = EPaletteRole::IrType;
            }
            else if ( IsOpcode( Word ) )
            {
                Role = EPaletteRole::IrOpcode;
            }

            Out.Emit( Word, Role );
            Cursor = End;
            continue;
        }

        // Whitespace runs as one span, everything else one character at a
        // time — the difference costs nothing and keeps the span count down on
        // the indentation every IR line begins with.
        if ( C == ' ' or C == '\t' or C == '\n' )
        {
            std::size_t End = Cursor;
            while ( End < Text.size() and ( Text[End] == ' ' or Text[End] == '\t' or Text[End] == '\n' ) )
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
