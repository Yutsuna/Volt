// ScopeResolver.cpp — Order 10 pass: binds every written identifier to its
// declaration site, once, and publishes the result on the PassContext.
//
// The pass only knows *value bindings* (parameters + locals): a name it
// cannot bind may be a type, a static call or a member — TypeChecker decides
// later, with type context in hand, so an unresolved identifier is never a
// diagnostic here (only a counter). The one diagnostic this pass owns is the
// redeclaration of a name in the same scope, a purely structural property.
// Shadowing a parent scope is legal and silent.
//
// One recursive visitor, no switch over node kinds: nodes that open a scope
// (Method, Block, If, While, the type-decl bodies) or declare/use a name
// (LocalDecl, Identifier, InstanceVar) get a lambda; everything else falls
// through the Reflect-driven field walk, so a new AST node with an ExprId or
// StmtList field is traversed for free.

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Sema/Pass.hpp"

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace Volt
{

namespace Sema
{

    namespace
    {

        class Resolver
        {

        public:

            explicit Resolver ( PassContext &InContext ) : Context( InContext )
            {
            }

            void Run ()
            {
                const ScopeId Root = Context.Scopes.PushScope( ScopeId{}, EScopeKind::Unit );
                for ( const Frontend::DeclId Id : Context.Ast.TopDecls )
                {
                    WalkDecl( Id, Root );
                }
                for ( const Frontend::StmtId Id : Context.Ast.TopStmts )
                {
                    WalkStmt( Id, Root );
                }
            }

        private:

            void Report ( Core::SourceRange Loc, std::string Message )
            {
                Context.Diags.Report( Core::Diagnostic{
                    .Severity = Core::ESeverity::Error, .Range = Loc, .Message = std::move( Message ), .Notes = {} } );
            }

            void DeclareOrReport ( ScopeId InScope, Symbol Name, BindingSite Site, Core::SourceRange Loc )
            {
                if ( not Context.Scopes.Declare( InScope, Name, Site ) )
                {
                    Report( Loc, "redeclaration of " + std::string{ Context.Ast.Text( Name ) } + " in the same scope" );
                }
            }

            // Params of a Method/Block declare into that scope; a default
            // value is evaluated where the scope opens, so it resolves there.
            void WalkParams ( const Frontend::ParamList &Params, ScopeId InScope )
            {
                for ( const Frontend::ParamId Id : Params )
                {
                    const Frontend::Param &Entry = Context.Ast.GetParam( Id );
                    DeclareOrReport( InScope, Entry.Name, BindingSite{ Id }, Entry.Loc );
                    WalkExpr( Entry.Default, InScope );
                }
            }

            // A type body is a structural scope only: members are resolved by
            // name-on-receiver (TypeStore::LookupMember), never through the
            // lexical chain, so nothing is declared into it here.
            void WalkTypeBody ( const Frontend::DeclList &Body, ScopeId Parent )
            {
                const ScopeId Inner = Context.Scopes.PushScope( Parent, EScopeKind::Type );
                for ( const Frontend::DeclId Child : Body )
                {
                    WalkDecl( Child, Inner );
                }
            }

            void WalkDecl ( Frontend::DeclId Id, ScopeId Current )
            {
                if ( not Id.IsValid() )
                {
                    return;
                }

                std::visit(
                    Meta::Overloaded{
                        [&] ( const Frontend::Module &Node )
                        {
                            const ScopeId Inner = Context.Scopes.PushScope( Current, EScopeKind::Unit );
                            for ( const Frontend::DeclId Child : Node.Body )
                            {
                                WalkDecl( Child, Inner );
                            }
                        },
                        [&] ( const Frontend::Class &Node ) { WalkTypeBody( Node.Body, Current ); },
                        [&] ( const Frontend::Struct &Node ) { WalkTypeBody( Node.Body, Current ); },
                        [&] ( const Frontend::Mixin &Node ) { WalkTypeBody( Node.Body, Current ); },
                        [&] ( const Frontend::Method &Node )
                        {
                            const ScopeId Inner = Context.Scopes.PushScope( Current, EScopeKind::Method );
                            WalkParams( Node.Params, Inner );
                            for ( const Frontend::StmtId Child : Node.Body )
                            {
                                WalkStmt( Child, Inner );
                            }
                        },
                        // A component is a type with parameters: its params
                        // are value bindings visible throughout its body.
                        [&] ( const Frontend::Component &Node )
                        {
                            const ScopeId Inner = Context.Scopes.PushScope( Current, EScopeKind::Type );
                            WalkParams( Node.Params, Inner );
                            for ( const Frontend::StmtId Child : Node.Body )
                            {
                                WalkStmt( Child, Inner );
                            }
                        },
                        [&] ( const Frontend::Circuit &Node )
                        {
                            const ScopeId Inner = Context.Scopes.PushScope( Current, EScopeKind::Type );
                            for ( const Frontend::StmtId Child : Node.Body )
                            {
                                WalkStmt( Child, Inner );
                            }
                        },
                        // Annotation arguments are compile-time metadata, not
                        // program values — never resolved, never counted.
                        [] ( const Frontend::Annotation & ) {},
                        [&] ( const auto &Node ) { WalkFields( Node, Current ); },
                    },
                    Context.Ast.Decl( Id ) );
            }

            void WalkStmt ( Frontend::StmtId Id, ScopeId Current )
            {
                if ( not Id.IsValid() )
                {
                    return;
                }
                Context.Scopes.SetScopeOf( Id, Current );

                std::visit(
                    Meta::Overloaded{
                        // Then and Else are separate Branch scopes: a Then
                        // local is never visible in Else.
                        [&] ( const Frontend::If &Node )
                        {
                            WalkExpr( Node.Cond, Current );
                            const ScopeId ThenScope = Context.Scopes.PushScope( Current, EScopeKind::Branch );
                            for ( const Frontend::StmtId Child : Node.Then )
                            {
                                WalkStmt( Child, ThenScope );
                            }
                            const ScopeId ElseScope = Context.Scopes.PushScope( Current, EScopeKind::Branch );
                            for ( const Frontend::StmtId Child : Node.Else )
                            {
                                WalkStmt( Child, ElseScope );
                            }
                        },
                        [&] ( const Frontend::While &Node )
                        {
                            WalkExpr( Node.Cond, Current );
                            const ScopeId BodyScope = Context.Scopes.PushScope( Current, EScopeKind::Branch );
                            for ( const Frontend::StmtId Child : Node.Body )
                            {
                                WalkStmt( Child, BodyScope );
                            }
                        },
                        // Declares in the current scope, does not open one:
                        // statement-order visibility falls out of the walk.
                        // The initializer resolves *before* the name exists,
                        // so `x = x + 1`-style inits see the outer binding.
                        [&] ( const Frontend::LocalDecl &Node )
                        {
                            WalkExpr( Node.Init, Current );
                            DeclareOrReport( Current, Node.Name, BindingSite{ Id }, Node.Loc );
                        },
                        [&] ( const auto &Node ) { WalkFields( Node, Current ); },
                    },
                    Context.Ast.Stmt( Id ) );
            }

            void CheckAndRecordCaptures ( ScopeId FromScope, const Binding &Found )
            {
                const bool bIsLocalVal = std::holds_alternative<Frontend::StmtId>( Found.Site ) or
                                         std::holds_alternative<Frontend::ParamId>( Found.Site );
                if ( not bIsLocalVal )
                {
                    return;
                }

                for ( ScopeId It = FromScope; It.IsValid() and It != Found.Owner; It = Context.Scopes.Get( It ).Parent )
                {
                    const Scope &CurrentScope = Context.Scopes.Get( It );
                    if ( CurrentScope.Kind == EScopeKind::Block )
                    {
                        Context.Scopes.RecordCapture( It, Found );
                    }
                }
            }

            // bDirectCallArg marks an expression written directly as a
            // call's trailing block / bare argument — the one syntactic
            // position a closure is guaranteed to be fully consumed before
            // the enclosing statement ends. It is not propagated into child
            // expressions: only the argument slot itself is a direct
            // position, not whatever it happens to contain.
            void WalkExpr ( Frontend::ExprId Id, ScopeId Current, bool bDirectCallArg = false )
            {
                if ( not Id.IsValid() )
                {
                    return;
                }

                std::visit(
                    Meta::Overloaded{
                        [&] ( const Frontend::Identifier &Node )
                        {
                            if ( const Binding *Found = Context.Scopes.Resolve( Current, Node.Name ) )
                            {
                                Context.Scopes.BindUse( Id, *Found );
                                CheckAndRecordCaptures( Current, *Found );
                                ++Context.Stats.ScopesResolved;
                                return;
                            }
                            // Not an error: may be a type name, a static
                            // method, a member — TypeChecker decides later.
                            ++Context.Stats.UnresolvedIdentifiers;
                        },
                        // `@name` is a member on self, resolved by
                        // name-on-receiver — never by the lexical chain.
                        [] ( const Frontend::InstanceVar & ) {},
                        [&] ( const Frontend::Block &Node )
                        {
                            const ScopeId Inner = Context.Scopes.PushScope( Current, EScopeKind::Block );
                            Context.Scopes.SetScopeOfExpr( Id, Inner );
                            Context.Scopes.SetEscapes( Inner, not bDirectCallArg );
                            WalkParams( Node.Params, Inner );
                            for ( const Frontend::StmtId Child : Node.Body )
                            {
                                WalkStmt( Child, Inner );
                            }
                        },
                        [&] ( const Frontend::Lambda &Node )
                        {
                            const ScopeId Inner = Context.Scopes.PushScope( Current, EScopeKind::Block );
                            Context.Scopes.SetScopeOfExpr( Id, Inner );
                            Context.Scopes.SetEscapes( Inner, not bDirectCallArg );
                            WalkParams( Node.Params, Inner );
                            WalkExpr( Node.Body, Inner );
                        },
                        [&] ( const Frontend::Call &Node )
                        {
                            WalkExpr( Node.Callee, Current );
                            for ( const Frontend::ExprId Arg : Node.Args )
                            {
                                WalkExpr( Arg, Current, /*bDirectCallArg=*/true );
                            }
                            WalkExpr( Node.BlockArg, Current, /*bDirectCallArg=*/true );
                        },
                        [&] ( const Frontend::DotCall &Node )
                        {
                            for ( const Frontend::ExprId Arg : Node.Args )
                            {
                                WalkExpr( Arg, Current, /*bDirectCallArg=*/true );
                            }
                        },
                        [&] ( const auto &Node ) { WalkFields( Node, Current ); },
                    },
                    Context.Ast.Expr( Id ) );
            }

            // Reflection-driven default walk: recurse into every child node a
            // field carries, whatever the node is called. This is what makes
            // adding an AST node free for this pass.
            template <typename NodeType> void WalkFields ( const NodeType &Node, ScopeId Current )
            {
                if constexpr ( Meta::Reflected<NodeType> )
                {
                    Meta::ForEachField( Node,
                                        [&] ( std::string_view, const auto &Field )
                                        {
                                            using FieldType = std::remove_cvref_t<decltype( Field )>;
                                            if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                                            {
                                                WalkExpr( Field, Current );
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
                                            {
                                                for ( const Frontend::ExprId Child : Field )
                                                {
                                                    WalkExpr( Child, Current );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, Frontend::StmtId> )
                                            {
                                                WalkStmt( Field, Current );
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, Frontend::StmtList> )
                                            {
                                                for ( const Frontend::StmtId Child : Field )
                                                {
                                                    WalkStmt( Child, Current );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, Frontend::DeclList> )
                                            {
                                                for ( const Frontend::DeclId Child : Field )
                                                {
                                                    WalkDecl( Child, Current );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, Frontend::ParamList> )
                                            {
                                                // Not a value scope (EnumCase
                                                // payload, MacroDef params):
                                                // only the defaults resolve.
                                                for ( const Frontend::ParamId Child : Field )
                                                {
                                                    WalkExpr( Context.Ast.GetParam( Child ).Default, Current );
                                                }
                                            }
                                        } );
                }
            }

            PassContext &Context;
        };

    } // namespace

    void ScopeResolver ( PassContext &Context )
    {
        Resolver Walk{ Context };
        Walk.Run();
    }

} // namespace Sema

} // namespace Volt
