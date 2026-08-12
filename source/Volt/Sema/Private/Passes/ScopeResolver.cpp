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

namespace
{

using namespace Volt;
using namespace Volt::Sema;

class Resolver
{

public:

    explicit Resolver ( PassContext &InContext ) : Context( InContext )
    {
    }

    void Run ()
    {
        const ScopeId Root = Context.Scopes.PushScope( ScopeId{}, EScopeKind::Unit );
        // Top-level statements first: a file is a module, and its top-level
        // locals are its globals (rules/core-ast.md's sibling decision) — a
        // `def` declared anywhere in the file can read them, so their binding
        // sites must exist before any `def` body is walked, not after.
        for ( const Frontend::StmtId Id : Context.Ast.TopStmts )
        {
            WalkStmt( Id, Root );
        }
        for ( const Frontend::DeclId Id : Context.Ast.TopDecls )
        {
            WalkDecl( Id, Root );
        }
    }

private:

    void Report ( Core::SourceRange Loc, std::string Message )
    {
        Context.Diags.Report(
            Core::Diagnostic{ .Severity = Core::ESeverity::Error, .Range = Loc, .Message = std::move( Message ), .Notes = {} } );
    }

    void DeclareOrReport ( ScopeId InScope, Symbol Name, BindingSite Site, Core::SourceRange Loc )
    {
        if ( not Context.Scopes.Declare( InScope, Name, Site ) )
        {
            Report( Loc, "redeclaration of " + std::string{ Context.Ast.Text( Name ) } + " in the same scope" );
        }
    }

    // The scope an implicit `name = expr` declares into: the nearest
    // enclosing scope that is not a bare lexical `Branch` (Then/Else of an
    // `If`, the body of a `While`, a `begin`/`ensure`/`rescue` body). Volt's
    // TypeChecker already implements a flat, Ruby-style local model per
    // *method* — `x = v if c` is visible after the `if`, not only inside its
    // Then — so ScopeResolver must agree, or the backend later refuses a
    // binding TypeChecker considers perfectly typed. Stops at `Block` too: a
    // closure body keeps its own assignments (RecordCapture depends on the
    // scope chain not skipping over it).
    [[nodiscard]] ScopeId NearestNonBranchScope ( ScopeId From ) const
    {
        ScopeId It = From;
        while ( It.IsValid() and Context.Scopes.Get( It ).Kind == EScopeKind::Branch )
        {
            It = Context.Scopes.Get( It ).Parent;
        }
        return It.IsValid() ? It : From;
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

    // A type body's own fields/methods are declared into this scope
    // (DeclId binding sites), but typing an access still goes
    // exclusively through name-on-receiver (TypeStore::LookupMember)
    // — this is binding metadata for tooling, not a second authority.
    // Two passes because member visibility is order-independent: a
    // method must see a field declared textually after it, unlike a
    // LocalDecl in a block.
    void WalkTypeBody ( const Frontend::DeclList &Body, ScopeId Parent )
    {
        const ScopeId Inner = Context.Scopes.PushScope( Parent, EScopeKind::Type );
        for ( const Frontend::DeclId Child : Body )
        {
            std::visit(
                Meta::Overloaded{
                    [&] ( const Frontend::Field &Node ) { Context.Scopes.Declare( Inner, Node.Name, BindingSite{ Child } ); },
                    [&] ( const Frontend::Method &Node ) { Context.Scopes.Declare( Inner, Node.Name, BindingSite{ Child } ); },
                    [] ( const auto & ) {},
                },
                Context.Ast.Decl( Child ) );
        }
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
                    // `external` and `abstract` are body-less: their parameters
                    // are never referenced in any statement, so registering them
                    // as bindings would always leave a use-count of zero and
                    // fire a false "unused parameter" warning. Only register
                    // params for methods that actually have a body to traverse.
                    if ( not Node.bExternal and not Node.bAbstract )
                    {
                        WalkParams( Node.Params, Inner );
                    }
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
                [&] ( const Frontend::While &Node )
                {
                    WalkExpr( Node.Cond, Current );
                    const ScopeId BodyScope = Context.Scopes.PushScope( Current, EScopeKind::Branch );
                    for ( const Frontend::StmtId Child : Node.Body )
                    {
                        WalkStmt( Child, BodyScope );
                    }
                },
                [&] ( const Frontend::RescueClause &Node )
                {
                    const ScopeId RescueScope = Context.Scopes.PushScope( Current, EScopeKind::Branch );
                    if ( Node.VarName.IsValid() )
                    {
                        DeclareOrReport( RescueScope, Node.VarName, BindingSite{ Id }, Node.Loc );
                    }
                    // Overrides the generic SetScopeOf( Id, Current ) above:
                    // unlike every other node with a StmtList body (While,
                    // If's Then/Else, BeginExpr's own Body/EnsureBody), a
                    // RescueClause declares a binding *of its own* directly
                    // in RescueScope, not just for its children — so
                    // ScopeOf( Id ) must resolve to the scope that actually
                    // owns that binding, or a StmtId-sited BindingSite{Id}
                    // consumer (BackendLLVM's SlotFor, which classifies
                    // unit-scope vs frame-local storage purely from
                    // ScopeOf( Site )) misreads a top-level `rescue e` as a
                    // module global — a real, previously-latent bug, only
                    // exposed once something first reads `e` inside a
                    // top-level clause body.
                    Context.Scopes.SetScopeOf( Id, RescueScope );
                    for ( const Frontend::StmtId Child : Node.Body )
                    {
                        WalkStmt( Child, RescueScope );
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
        if ( not IsValueBinding( Found.Site ) )
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
                // `@name` is a member on self, typed by
                // name-on-receiver (MemberType/TypeStore) — never by
                // the lexical chain. Resolving it here only records a
                // best-effort binding for tooling; it reaches an
                // own-body field declared in WalkTypeBody through the
                // Method scope's Type parent, never an inherited one
                // (a superclass/mixin body lives in a different
                // DeclList). A miss is not an error.
                [&] ( const Frontend::InstanceVar &Node )
                {
                    if ( const Binding *Found = Context.Scopes.Resolve( Current, Node.Name ) )
                    {
                        Context.Scopes.BindUse( Id, *Found );
                    }
                },
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
                // The main body and `ensure` are separate Branch
                // scopes, same as If/While — a local declared inside
                // `begin ... end` must not leak into the enclosing
                // scope. Each RescueClause pushes and populates its
                // own Branch scope already (WalkStmt above), so its
                // clauses are simply walked in Current here.
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
                [&] ( const Frontend::BeginExpr &Node )
                {
                    const ScopeId BodyScope = Context.Scopes.PushScope( Current, EScopeKind::Branch );
                    for ( const Frontend::StmtId Child : Node.Body )
                    {
                        WalkStmt( Child, BodyScope );
                    }
                    for ( const Frontend::StmtId Clause : Node.RescueClauses )
                    {
                        WalkStmt( Clause, Current );
                    }
                    const ScopeId EnsureScope = Context.Scopes.PushScope( Current, EScopeKind::Branch );
                    for ( const Frontend::StmtId Child : Node.EnsureBody )
                    {
                        WalkStmt( Child, EnsureScope );
                    }
                },
                // `name = expr` with no `: Type` is a *declaration* the parser
                // could not recognise as one: telling it apart from a
                // reassignment needs the scope chain, which exists only here.
                // So this is the second place a local is declared, and the
                // only one whose site is an expression — the Target itself
                // (ScopeTable.hpp, BindingSite).
                //
                // The value resolves first, exactly as a LocalDecl's
                // initialiser does, so `x = x + 1` sees the outer binding. A
                // name already bound to *storage* (a local or a parameter,
                // here or in an enclosing scope) is an ordinary reassignment;
                // one bound only to a member declaration is not, since a field
                // is assignable through `@name` alone — so `count = 0` in the
                // body of a type that has a `count` member declares a local
                // that shadows it, and only fails to when that member sits in
                // this very scope, where shadowing is not expressible.
                [&] ( const Frontend::Assign &Node )
                {
                    WalkExpr( Node.Value, Current );

                    if ( Node.Op == Frontend::TokenKind::Assign and Node.Target.IsValid() )
                    {
                        const auto *Target   = std::get_if<Frontend::Identifier>( &Context.Ast.Expr( Node.Target ) );
                        const Binding *Found = Target != nullptr ? Context.Scopes.Resolve( Current, Target->Name ) : nullptr;
                        if ( Target != nullptr and ( Found == nullptr or not IsValueBinding( Found->Site ) ) )
                        {
                            // `x = v if c` parses as `If{ Then: [Assign] }`
                            // (ApplyModifiers) — Current is the Then's own
                            // Branch scope, but TypeChecker's locals are flat
                            // per method, so the declaration must land where
                            // TypeChecker will actually see it live on: the
                            // nearest enclosing Method/Block/Type/Unit.
                            const ScopeId DeclScope = NearestNonBranchScope( Current );
                            if ( Context.Scopes.Declare( DeclScope, Target->Name, BindingSite{ Node.Target } ) )
                            {
                                // Bound, so every consumer reaches the new binding
                                // through BindingOf like any other; not counted, so
                                // a variable nothing ever reads is still unused.
                                if ( const Binding *Declared = Context.Scopes.Resolve( Current, Target->Name ) )
                                {
                                    Context.Scopes.BindUse( Node.Target, *Declared, /*bCountsAsUse=*/false );
                                    ++Context.Stats.ScopesResolved;
                                }
                                // `SlotFor` (StmtEmitter.cpp) routes a global
                                // vs. a frame `alloca` by asking this exact
                                // question of the *site* — never of `Current`,
                                // which would still say `Branch`.
                                Context.Scopes.SetScopeOfExpr( Node.Target, DeclScope );
                                return;
                            }
                        }
                    }
                    WalkExpr( Node.Target, Current );
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

    Volt::Sema::PassContext &Context;
};

} // namespace

void Volt::Sema::ScopeResolver ( Volt::Sema::PassContext &Context )
{
    Resolver Walk{ Context };
    Walk.Run();
}
