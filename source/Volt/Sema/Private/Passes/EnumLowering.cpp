// EnumLowering.cpp — order 12. Materializes implicit enum-case values:
//
//   enum Color
//       Red
//       Green
//       Blue
//   end
//
// becomes (in place) `Red = 0`, `Green = 1`, `Blue = 2` — synthesized
// `IntLiteral` expressions assigned to `EnumCase::Value`. A case that already
// carries an explicit value (`Ok = 200`) is left untouched, and resets the
// running counter to `Value + 1` for the cases that follow. This is purely
// syntactic (no type information is needed) and must run before
// MacroExpansion so a macro-generated enum body still sees consistent
// numbering downstream — it does not touch `Enum::Underlying`, which stays
// whatever the parser produced (invalid unless `: Type` was written): that
// binding is a TypeChecker concern once stdlib annotations resolve it,
// never guessed here (zero-hardcode).

#include "Volt/Frontend/AST/AstContext.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Sema/Pass.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace Volt
{

namespace Sema
{

    namespace
    {

        using namespace Frontend;

        class EnumRewriter
        {

        public:

            explicit EnumRewriter ( AstContext &InContext ) : Context( InContext )
            {
            }

            /// Lower every enum present in the context. Returns the number of
            /// `Enum` decls processed (not the number of cases materialized).
            std::size_t Run ()
            {
                // Only decls present before lowering can be an Enum; cases we
                // synthesize land in the expr arena, never the decl arena, so
                // the decl count never grows here and no re-scan is needed.
                const std::size_t OriginalDeclCount = Context.DeclCount();
                std::size_t Lowered                 = 0;

                for ( std::size_t Index = 0; Index < OriginalDeclCount; ++Index )
                {
                    const DeclId Id{ static_cast<DeclId::ValueType>( Index ) };
                    if ( KindOf( Context.Decl( Id ) ) == DeclKind::Enum )
                    {
                        LowerEnum( Id );
                        ++Lowered;
                    }
                }

                return Lowered;
            }

        private:

            /// Best-effort decode of a raw `IntLiteral` lexeme. Non-numeric or
            /// unparseable text leaves Out untouched: this pass is syntactic
            /// and never diagnoses — that is TypeChecker's job.
            [[nodiscard]] bool TryDecodeInt ( ExprId Id, std::int64_t &Out ) const
            {
                if ( !Id.IsValid() or KindOf( Context.Expr( Id ) ) != ExprKind::IntLiteral )
                {
                    return false;
                }

                const std::string_view Text = Context.Text( std::get<IntLiteral>( Context.Expr( Id ) ).Raw );
                std::int64_t Value          = 0;
                const auto [Ptr, Error]     = std::from_chars( Text.data(), Text.data() + Text.size(), Value );
                if ( Error != std::errc{} )
                {
                    return false;
                }

                Out = Value;
                return true;
            }

            [[nodiscard]] ExprId MakeIntLiteral ( std::int64_t Value, Core::SourceRange Loc )
            {
                IntLiteral Node;
                Node.Loc = Loc;
                Node.Raw = Context.Strings().Intern( std::to_string( Value ) );
                return Context.Add( ExprNode{ Node } );
            }

            void LowerEnum ( DeclId EnumId )
            {
                // Enum::Body only ever holds pre-existing DeclIds — the
                // pass mutates the EnumCase slots in place and never adds or
                // reorders entries, so the Enum decl itself never changes and
                // does not need copy-out / write-back.
                const DeclList &Body = std::get<Enum>( Context.Decl( EnumId ) ).Body;

                std::int64_t NextImplicit = 0;

                for ( const DeclId CaseId : Body )
                {
                    if ( !CaseId.IsValid() or KindOf( Context.Decl( CaseId ) ) != DeclKind::EnumCase )
                    {
                        continue;
                    }

                    EnumCase Case = std::get<EnumCase>( Context.Decl( CaseId ) );

                    if ( Case.Value.IsValid() )
                    {
                        std::int64_t Explicit = 0;
                        if ( TryDecodeInt( Case.Value, Explicit ) )
                        {
                            NextImplicit = Explicit + 1;
                        }
                        // Non-literal explicit values (out of scope for this
                        // syntactic pass) leave NextImplicit unchanged.
                        continue;
                    }

                    Case.Value             = MakeIntLiteral( NextImplicit, Case.Loc );
                    Context.Decl( CaseId ) = DeclNode{ std::move( Case ) };
                    ++NextImplicit;
                }
            }

            AstContext &Context;
        };

    } // namespace

    // Order 12 — materialize implicit enum-case values before macro
    // expansion / case lowering ever see the tree.
    void EnumLowering ( PassContext &Context )
    {
        Context.Stats.EnumsLowered += EnumRewriter( Context.Ast ).Run();
    }

} // namespace Sema

} // namespace Volt
