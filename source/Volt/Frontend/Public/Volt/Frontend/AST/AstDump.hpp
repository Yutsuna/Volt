#pragma once

#include "Volt/Core/Diagnostics/SourceManager.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/AstPrinter.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"

#include <cctype>
#include <cstddef>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Volt
{

namespace Frontend
{

    /**
     * @struct FAstDumpOptions
     * @brief Rendering switches for the human AST tree dump.
     */
    struct FAstDumpOptions
    {

        bool bColor        = false;
        bool bShowLocation = true;
    };

    namespace Detail
    {

        /// "ReturnType" -> "return_type" (the tree's child-label convention).
        [[nodiscard]] inline std::string SnakeCase ( std::string_view Name )
        {
            std::string Out;
            Out.reserve( Name.size() + 4 );
            for ( std::size_t Index = 0; Index < Name.size(); ++Index )
            {
                const unsigned char Letter = static_cast<unsigned char>( Name[Index] );
                if ( std::isupper( Letter ) != 0 )
                {
                    if ( Index > 0 )
                    {
                        Out.push_back( '_' );
                    }
                    Out.push_back( static_cast<char>( std::tolower( Letter ) ) );
                }
                else
                {
                    Out.push_back( static_cast<char>( Letter ) );
                }
            }
            return Out;
        }

        /// "bSelfClosing" -> "SelfClosing": drop the bool Hungarian prefix.
        [[nodiscard]] inline std::string_view StripBoolPrefix ( std::string_view Name )
        {
            if ( Name.size() > 1 and Name[0] == 'b' and std::isupper( static_cast<unsigned char>( Name[1] ) ) != 0 )
            {
                return Name.substr( 1 );
            }
            return Name;
        }

    } // namespace Detail

    /// Human tree dump of an AstContext, in the historical `volt parse` style:
    ///
    ///   Program 'file.vl'
    ///   ├─ Method 'square' @7:3
    ///   │  ├─ params
    ///   │  │  └─ Param 'x' @7:15
    ///   │  └─ body
    ///   │     └─ ...
    ///
    /// Like AstPrinter it is fully reflective: node headers, inline atoms and
    /// labeled children all fall out of the field walk — zero per-node code.
    class AstDumper
    {

    public:

        AstDumper ( const AstContext &InContext,
                    const Core::SourceManager &InSources,
                    std::ostream &InOut,
                    FAstDumpOptions InOptions = {} )
            : Context( InContext ), Sources( InSources ), Out( InOut ), Options( InOptions )
        {
        }

        /// `Program '<path>'` root followed by every top-level decl and stmt.
        void DumpFile ()
        {
            Out << NodeText( "Program" ) << ' ' << ValueText( Quoted( Sources.PathOf( Context.FileId() ) ) ) << '\n';

            const std::size_t Total = Context.TopDecls.size() + Context.TopStmts.size();
            std::size_t Index       = 0;
            for ( const DeclId Id : Context.TopDecls )
            {
                const bool bLast = ++Index == Total;
                Out << Branch( bLast );
                Dump( Id, Vert( bLast ) );
            }
            for ( const StmtId Id : Context.TopStmts )
            {
                const bool bLast = ++Index == Total;
                Out << Branch( bLast );
                Dump( Id, Vert( bLast ) );
            }
        }

        void Dump ( ExprId Id, const std::string &Prefix )
        {
            DumpVariant( Context.Expr( Id ), Prefix );
        }

        void Dump ( StmtId Id, const std::string &Prefix )
        {
            DumpVariant( Context.Stmt( Id ), Prefix );
        }

        void Dump ( DeclId Id, const std::string &Prefix )
        {
            DumpVariant( Context.Decl( Id ), Prefix );
        }

        void Dump ( TypeId Id, const std::string &Prefix )
        {
            DumpVariant( Context.Type( Id ), Prefix );
        }

        void Dump ( ParamId Id, const std::string &Prefix )
        {
            DumpNode( "Param", Context.GetParam( Id ), Prefix );
        }

    private:

        // --- Tree glyphs and palette -------------------------------------

        static constexpr std::string_view GlyphBranch = "├─ ";
        static constexpr std::string_view GlyphCorner = "└─ ";
        static constexpr std::string_view GlyphVert   = "│  ";
        static constexpr std::string_view GlyphSpace  = "   ";

        static constexpr std::string_view AnsiReset = "\x1b[0m";
        static constexpr std::string_view AnsiNode  = "\x1b[1;37m"; //<< node names: bold white
        static constexpr std::string_view AnsiValue = "\x1b[36m";   //<< values: cyan
        static constexpr std::string_view AnsiKey   = "\x1b[90m";   //<< modifier keywords: dark gray
        static constexpr std::string_view AnsiLabel = "\x1b[34m";   //<< child labels: blue
        static constexpr std::string_view AnsiDim   = "\x1b[90m";   //<< glyphs and locations: dark gray

        [[nodiscard]] std::string Paint ( std::string_view Code, std::string_view Text ) const
        {
            if ( !Options.bColor )
            {
                return std::string( Text );
            }
            std::string Result;
            Result.reserve( Code.size() + Text.size() + AnsiReset.size() );
            Result += Code;
            Result += Text;
            Result += AnsiReset;
            return Result;
        }

        [[nodiscard]] std::string NodeText ( std::string_view Text ) const
        {
            return Paint( AnsiNode, Text );
        }

        [[nodiscard]] std::string ValueText ( std::string_view Text ) const
        {
            return Paint( AnsiValue, Text );
        }

        [[nodiscard]] std::string KeyText ( std::string_view Text ) const
        {
            return Paint( AnsiKey, Text );
        }

        [[nodiscard]] std::string LabelText ( std::string_view Text ) const
        {
            return Paint( AnsiLabel, Text );
        }

        [[nodiscard]] std::string Branch ( bool bLast ) const
        {
            return Paint( AnsiDim, bLast ? GlyphCorner : GlyphBranch );
        }

        [[nodiscard]] std::string Vert ( bool bLast ) const
        {
            return Paint( AnsiDim, bLast ? GlyphSpace : GlyphVert );
        }

        [[nodiscard]] static std::string Quoted ( std::string_view Text )
        {
            std::string Result;
            Result.reserve( Text.size() + 2 );
            Result += '\'';
            Result += Text;
            Result += '\'';
            return Result;
        }

        // --- Field classification ----------------------------------------

        template <typename F>
        static constexpr bool IsNodeId = std::same_as<F, ExprId> or std::same_as<F, StmtId> or std::same_as<F, DeclId> ||
                                         std::same_as<F, TypeId> or std::same_as<F, ParamId>;

        // Element type of a NodeList, or void for anything else — keeps the
        // classification below well-formed for every reflected field type.
        template <typename F> struct TListElem
        {
            using Type = void;
        };

        template <typename T, std::size_t N> struct TListElem<Core::SmallVec<T, N>>
        {
            using Type = T;
        };

        template <typename F> static constexpr bool IsSymbolList = std::same_as<typename TListElem<F>::Type, Symbol>;

        template <typename F> static constexpr bool IsIdList = Detail::IsNodeList<F>::value and !IsSymbolList<F>;

        // --- Node rendering ----------------------------------------------

        template <typename Variant> void DumpVariant ( const Variant &Node, const std::string &Prefix )
        {
            std::visit( Meta::Overloaded{ [&] ( const std::monostate & ) { Out << NodeText( "None" ) << '\n'; },
                                          [&] ( const auto &Alt )
                                          { DumpNode( Meta::TypeName<decltype( Alt )>(), Alt, Prefix ); } },
                        Node );
        }

        /// The caller has already emitted this node's branch glyph; print the
        /// header line (name + inline atoms + location), then every child
        /// under Prefix.
        template <typename TNode> void DumpNode ( std::string_view Name, const TNode &Node, const std::string &Prefix )
        {
            Out << NodeText( Name );
            Meta::ForEachField( Node, [&] ( const char *FieldName, const auto &Field ) { PrintInlineAtom( FieldName, Field ); } );
            if constexpr ( requires { Node.Loc; } )
            {
                PrintLocation( Node.Loc );
            }
            Out << '\n';

            std::vector<std::function<void( const std::string &, bool )>> Children;
            Meta::ForEachField( Node, [&] ( const char *FieldName, const auto &Field )
                                { CollectChild( Children, FieldName, Field ); } );

            for ( std::size_t Index = 0; Index < Children.size(); ++Index )
            {
                Children[Index]( Prefix, Index + 1 == Children.size() );
            }
        }

        void PrintLocation ( const Core::SourceRange &Loc )
        {
            // Synthesized nodes carry an invalid file; SourceManager lookups
            // are not bounds-checked, so skip rather than resolve.
            if ( !Options.bShowLocation or !Loc.File.IsValid() )
            {
                return;
            }
            const Core::LineColumn Position = Sources.Resolve( Loc.File, Loc.Begin );
            Out << Paint( AnsiDim, " @" + std::to_string( Position.Line ) + ':' + std::to_string( Position.Column ) );
        }

        /// Scalar fields shown on the header line: names, operators, flags.
        template <typename F> void PrintInlineAtom ( const char *FieldName, const F &Field )
        {
            if constexpr ( std::same_as<F, Symbol> )
            {
                if ( Field.IsValid() )
                {
                    Out << ' ' << ValueText( Quoted( Context.Text( Field ) ) );
                }
            }
            else if constexpr ( std::same_as<F, bool> )
            {
                // `Value` is data (BoolLiteral) and always prints; the
                // b-prefixed modifiers print as bare keywords when set.
                if ( std::string_view( FieldName ) == "Value" )
                {
                    Out << ' ' << ValueText( Field ? "true" : "false" );
                }
                else if ( Field )
                {
                    Out << ' ' << KeyText( Detail::SnakeCase( Detail::StripBoolPrefix( FieldName ) ) );
                }
            }
            else if constexpr ( std::same_as<F, TokenKind> )
            {
                // Eof is the unset sentinel (Section::Op when Kind isn't
                // Operator) — skip it, mirroring Symbol::IsValid() above.
                if ( Field != TokenKind::Eof )
                {
                    const std::string_view Spelling = TokenSpelling( Field );
                    Out << ' ' << ValueText( Quoted( Spelling.empty() ? TokenName( Field ) : Spelling ) );
                }
            }
            else if constexpr ( std::is_enum_v<F> )
            {
                const std::string_view EnumText = Meta::EnumName( Field );
                if ( EnumText != "None" )
                {
                    Out << ' ' << KeyText( Detail::SnakeCase( EnumText ) );
                }
            }
            else if constexpr ( IsSymbolList<F> )
            {
                // A qualified path prints as one name; other symbol lists
                // (generics, attribute names) as a bracketed group. Invalid
                // symbols (e.g. positional slots in Call::ArgNames) are
                // placeholders, not text — skip them.
                const bool bPath          = std::string_view( FieldName ) == "Path";
                const std::string_view In = bPath ? "::" : ", ";
                std::string Joined;
                bool bAny = false;
                for ( std::size_t Index = 0; Index < Field.Size(); ++Index )
                {
                    if ( !Field[Index].IsValid() )
                    {
                        continue;
                    }
                    if ( bAny )
                    {
                        Joined += In;
                    }
                    Joined += Context.Text( Field[Index] );
                    bAny = true;
                }
                if ( bAny )
                {
                    Out << ' ' << ( bPath ? ValueText( Quoted( Joined ) ) : KeyText( "[" + Joined + "]" ) );
                }
            }
            else
            {
                static_cast<void>( FieldName );
                static_cast<void>( Field );
            }
        }

        /// Node references and node lists become labeled child branches.
        template <typename F>
        void CollectChild ( std::vector<std::function<void( const std::string &, bool )>> &Children,
                            const char *FieldName,
                            const F &Field )
        {
            if constexpr ( IsNodeId<F> )
            {
                if ( Field.IsValid() )
                {
                    Children.push_back( [this, FieldName, Field] ( const std::string &Prefix, bool bLast )
                                        { EmitSingle( Detail::SnakeCase( FieldName ), Field, Prefix, bLast ); } );
                }
            }
            else if constexpr ( IsIdList<F> )
            {
                if ( !Field.IsEmpty() )
                {
                    Children.push_back( [this, FieldName, &Field] ( const std::string &Prefix, bool bLast )
                                        { EmitList( Detail::SnakeCase( FieldName ), Field, Prefix, bLast ); } );
                }
            }
            else
            {
                static_cast<void>( Children );
                static_cast<void>( FieldName );
                static_cast<void>( Field );
            }
        }

        template <typename TId> void EmitSingle ( const std::string &Label, TId Id, const std::string &Prefix, bool bLast )
        {
            Out << Prefix << Branch( bLast ) << LabelText( Label ) << '\n';
            const std::string Sub = Prefix + Vert( bLast );
            Out << Sub << Branch( true );
            Dump( Id, Sub + Vert( true ) );
        }

        template <typename TList>
        void EmitList ( const std::string &Label, const TList &List, const std::string &Prefix, bool bLast )
        {
            Out << Prefix << Branch( bLast ) << LabelText( Label ) << '\n';
            const std::string Sub = Prefix + Vert( bLast );
            for ( std::size_t Index = 0; Index < List.Size(); ++Index )
            {
                const bool bLastElem = Index + 1 == List.Size();
                Out << Sub << Branch( bLastElem );
                Dump( List[Index], Sub + Vert( bLastElem ) );
            }
        }

        const AstContext &Context;
        const Core::SourceManager &Sources;
        std::ostream &Out;
        FAstDumpOptions Options;
    };

} // namespace Frontend

} // namespace Volt
