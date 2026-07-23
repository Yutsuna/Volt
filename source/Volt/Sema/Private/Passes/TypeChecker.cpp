// TypeChecker.cpp — Order 30 pass: gives every expression a type, and
// resolves the members that types make available.
//
// In Volt there are no bare machine values: `10` is whatever type claimed the
// IntLiteral node, `[ 1, 2 ]` whatever claimed ArrayLit — instantiated with
// the type of its elements. Which type that is comes entirely from the
// stdlib's `@[Literal( ... )]` annotations, bound before this pass runs, and
// what `.to_string` or `+` mean comes from the members the binder published.
// No Volt type name and no literal node name appears in this file: the node
// kind is reflected off the AST, the type is whatever claimed it.

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Stmt.hpp"
#include "Volt/Frontend/Lexer/Token.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"
#include "Volt/Sema/Pass.hpp"

#if VOLT_HAS_REFLECTION
    #include <meta>
#endif
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace Volt
{

namespace Sema
{

    namespace
    {

        // The member name an `Object[ ... ]` access resolves to. Indexing is
        // an operator like any other, and its spelling is an AST property —
        // the method behind it lives in source/Lib/ as `def []`.
        constexpr std::string_view IndexOperator = "[]";

        // Whether a node carries any child node at all. A node with payload
        // but no children is a *leaf literal*: a written value the stdlib is
        // expected to claim. Anything with children is structure, and its
        // absence from the node-kind table is simply not a literal.
        template <typename NodeType> [[nodiscard]] consteval bool HasChildNodes ()
        {
#if VOLT_HAS_REFLECTION
            for ( const auto Field : std::meta::nonstatic_data_members_of( ^^NodeType, std::meta::access_context::unchecked() ) )
            {
                const auto FieldType = std::meta::dealias( std::meta::type_of( Field ) );
                if ( FieldType == std::meta::dealias( ^^Frontend::ExprId ) or
                     FieldType == std::meta::dealias( ^^Frontend::ExprList ) or
                     FieldType == std::meta::dealias( ^^Frontend::TypeId ) or
                     FieldType == std::meta::dealias( ^^Frontend::TypeList ) or
                     FieldType == std::meta::dealias( ^^Frontend::StmtList ) or
                     FieldType == std::meta::dealias( ^^Frontend::DeclList ) or
                     FieldType == std::meta::dealias( ^^Frontend::ParamList ) )
                {
                    return true;
                }
            }
            return false;
#else
            return false;
#endif
        }

        // Annotation arguments are compile-time metadata, not program values:
        // the "libc" in `@[External( "libc" )]` names a library, it is not a
        // Volt String. Mark those subtrees so inference skips them.
        //
        // Reflection walks the children, so a new node with expression fields
        // is covered without touching this pass.
        void MarkMetadata ( const Frontend::AstContext &Ast, Frontend::ExprId Id, std::vector<bool> &Marked )
        {
            if ( not Id.IsValid() or Id.Value >= Marked.size() or Marked[Id.Value] )
            {
                return;
            }
            Marked[Id.Value] = true;

            std::visit(
                [&] ( const auto &Concrete )
                {
                    if constexpr ( Meta::Reflected<decltype( Concrete )> )
                    {
                        Meta::ForEachField( Concrete,
                                            [&] ( std::string_view, const auto &Field )
                                            {
                                                using FieldType = std::remove_cvref_t<decltype( Field )>;
                                                if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                                                {
                                                    MarkMetadata( Ast, Field, Marked );
                                                }
                                                else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
                                                {
                                                    for ( const Frontend::ExprId Child : Field )
                                                    {
                                                        MarkMetadata( Ast, Child, Marked );
                                                    }
                                                }
                                            } );
                    }
                },
                Ast.Expr( Id ) );
        }

        [[nodiscard]] std::vector<bool> MetadataExprs ( const Frontend::AstContext &Ast )
        {
            std::vector<bool> Marked( Ast.ExprCount(), false );

            const std::size_t Count = Ast.DeclCount();
            for ( std::size_t Index = 0; Index < Count; ++Index )
            {
                const Frontend::DeclId Id{ static_cast<std::uint32_t>( Index ) };
                if ( const auto *Anno = std::get_if<Frontend::Annotation>( &Ast.Decl( Id ) ) )
                {
                    for ( const Frontend::ExprId Arg : Anno->Args )
                    {
                        MarkMetadata( Ast, Arg, Marked );
                    }
                }
            }
            return Marked;
        }

        // A member resolved on a receiver, with its already-instantiated
        // result type and (for a method) its already-instantiated parameter
        // types, in declaration order.
        struct Resolution
        {

            const Member *Decl = nullptr;
            SemaTypeId Result;
            Core::SmallVec<SemaTypeId, 4> Params;
        };

        struct Checker
        {

            PassContext &Ctx;
            std::vector<bool> Metadata;
            std::unordered_set<std::uint32_t> NakedTypeExprs{};
            bool bStaticContext = false;

            // The type whose body we are inside, and the AST symbols of its
            // generic parameters (matched by ResolveTypeExpr, which reads
            // written names out of *this* unit's interner).
            NominalId SelfType{};
            const Frontend::SymbolList *SelfGenerics = nullptr;
            SemaTypeId SelfValue{};

            // Locals of the method body being walked, keyed by the
            // declaration site ScopeResolver published: structural keys, so
            // two locals in sibling scopes can never collide.
            std::unordered_map<BindingSite, SemaTypeId, BindingSiteHash> LocalTypes{};
            // Legacy name-keyed fallback, kept for the identifiers the
            // ScopeTable cannot know: nodes materialised after Order 10
            // (MacroExpansion, CaseLowering) and implicit `x = 5` assigns.
            std::unordered_map<Symbol, SemaTypeId> Locals{};
            std::unordered_set<std::uint32_t> UnconstrainedLiterals{};
            std::unordered_map<Symbol, Frontend::ExprId> UnconstrainedVarInitializers{};
            SemaTypeId CurrentMethodReturnType{};

            // The resolution behind a `receiver.name` expression that turned
            // out to be a method, keyed by that Member expression's own Id —
            // filled when it is inferred, read back by the wrapping Call so
            // arguments can be checked against Member::Params without a
            // second lookup.
            std::unordered_map<std::uint32_t, Resolution> CalleeResolution{};

            void Report ( Core::SourceRange Loc, std::string Message )
            {
                Ctx.Diags.Report( Core::Diagnostic{
                    .Severity = Core::ESeverity::Error, .Range = Loc, .Message = std::move( Message ), .Notes = {} } );
            }

            // The type a used identifier resolved to, if any: the site-keyed
            // table when ScopeResolver bound the use (structurally exact,
            // sibling scopes never collide), the legacy name map otherwise.
            // An engaged-but-invalid result means "declared, type unknown" —
            // distinct from "no local of that name".
            [[nodiscard]] std::optional<SemaTypeId> FindLocal ( Frontend::ExprId Use, Symbol Name ) const
            {
                if ( const Binding *Bound = Ctx.Scopes.BindingOf( Use ) )
                {
                    if ( const auto It = LocalTypes.find( Bound->Site ); It != LocalTypes.end() )
                    {
                        return It->second;
                    }
                }
                if ( const auto It = Locals.find( Name ); It != Locals.end() )
                {
                    return It->second;
                }
                return std::nullopt;
            }

            // Both tables move together: the site key when ScopeResolver
            // bound this use, the name key always (legacy fallback path).
            void WriteLocal ( Frontend::ExprId Use, Symbol Name, SemaTypeId Type )
            {
                if ( const Binding *Bound = Ctx.Scopes.BindingOf( Use ) )
                {
                    LocalTypes[Bound->Site] = Type;
                    Ctx.Values.SetSiteType( Bound->Site, Type );
                }
                Locals[Name] = Type;
            }

            void ConstrainExprType ( Frontend::ExprId Expr, SemaTypeId TargetType )
            {
                if ( not Expr.IsValid() or not TargetType.IsValid() )
                {
                    return;
                }

                if ( UnconstrainedLiterals.contains( Expr.Value ) )
                {
                    Ctx.Values.SetExprType( Expr, TargetType );
                    UnconstrainedLiterals.erase( Expr.Value );
                    return;
                }

                const auto &Node = Ctx.Ast.Expr( Expr );

                if ( const auto *IdNode = std::get_if<Frontend::Identifier>( &Node ) )
                {
                    Ctx.Values.SetExprType( Expr, TargetType );
                    if ( const auto It = UnconstrainedVarInitializers.find( IdNode->Name );
                         It != UnconstrainedVarInitializers.end() )
                    {
                        const Frontend::ExprId InitExpr = It->second;
                        UnconstrainedVarInitializers.erase( It );
                        WriteLocal( Expr, IdNode->Name, TargetType );
                        ConstrainExprType( InitExpr, TargetType );
                    }
                    return;
                }

                if ( const auto *Ternary = std::get_if<Frontend::Ternary>( &Node ) )
                {
                    ConstrainExprType( Ternary->Then, TargetType );
                    ConstrainExprType( Ternary->Else, TargetType );
                    Ctx.Values.SetExprType( Expr, TargetType );
                    return;
                }

                if ( const auto *Binary = std::get_if<Frontend::Binary>( &Node ) )
                {
                    ConstrainExprType( Binary->Lhs, TargetType );
                    ConstrainExprType( Binary->Rhs, TargetType );
                    Ctx.Values.SetExprType( Expr, TargetType );
                    return;
                }
            }

            [[nodiscard]] std::span<const Symbol> Generics () const
            {
                if ( SelfGenerics == nullptr )
                {
                    return {};
                }
                return std::span<const Symbol>{ SelfGenerics->begin(), SelfGenerics->Size() };
            }

            // The printable name of a type, always taken from the store —
            // never a string this file knows.
            [[nodiscard]] std::string NameOf ( NominalId Id ) const
            {
                if ( not Id.IsValid() )
                {
                    return "<unresolved>";
                }
                return std::string{ Ctx.Types.Text( Ctx.Types.Type( Id ).Name ) };
            }

            // The printable name of an expression's type, always taken from
            // the store through the unit's own value table.
            [[nodiscard]] std::string NameOfValue ( SemaTypeId Id ) const
            {
                if ( not Ctx.Values.Has( Id ) )
                {
                    return "<unresolved>";
                }
                return NameOf( Ctx.Values.Get( Id ).Base );
            }

            [[nodiscard]] SemaTypeId MakeType ( NominalId Base, Core::SmallVec<SemaTypeId, 2> Args )
            {
                return Ctx.Values.Intern( SemaType{ .Base = Base, .Args = std::move( Args ) } );
            }

            // --- Declarations ---------------------------------------------

            template <typename DeclContainer> void WalkDecls ( const DeclContainer &Decls )
            {
                for ( const Frontend::DeclId Id : Decls )
                {
                    WalkDecl( Id );
                }
            }

            void EnterType ( NominalId Id, const Frontend::SymbolList &Params, const Frontend::DeclList &Body )
            {
                const NominalId OuterType                 = SelfType;
                const Frontend::SymbolList *OuterGenerics = SelfGenerics;
                const SemaTypeId OuterValue               = SelfValue;

                SelfType     = Id;
                SelfGenerics = &Params;

                // `self` inside a generic body is that generic applied to its
                // own parameters — which are not concrete, so the arguments
                // stay unresolved. Member lookup still works; the parameter
                // dependent parts simply stay untyped.
                Core::SmallVec<SemaTypeId, 2> Args;
                for ( std::size_t Index = 0; Index < Params.Size(); ++Index )
                {
                    Args.PushBack( SemaTypeId{} );
                }
                SelfValue = MakeType( Id, std::move( Args ) );

                WalkDecls( Body );

                SelfType     = OuterType;
                SelfGenerics = OuterGenerics;
                SelfValue    = OuterValue;
            }

            void EnterMethod ( const Frontend::Method &Node )
            {
                std::unordered_map<BindingSite, SemaTypeId, BindingSiteHash> OuterLocalTypes;
                OuterLocalTypes.swap( LocalTypes );
                std::unordered_map<Symbol, SemaTypeId> OuterLocals;
                OuterLocals.swap( Locals );
                std::unordered_set<std::uint32_t> OuterUnconstrainedLiterals;
                OuterUnconstrainedLiterals.swap( UnconstrainedLiterals );
                std::unordered_map<Symbol, Frontend::ExprId> OuterUnconstrainedVarInitializers;
                OuterUnconstrainedVarInitializers.swap( UnconstrainedVarInitializers );
                const SemaTypeId OuterReturnType = CurrentMethodReturnType;

                UnitSink Sink{ .Values = Ctx.Values, .Self = SelfValue };
                CurrentMethodReturnType = ResolveTypeExpr( Ctx.Ast, Ctx.Types, Generics(), Sink, Node.ReturnType );

                const bool bOuterStatic = bStaticContext;
                bStaticContext          = Node.bSelf;

                for ( const Frontend::ParamId Id : Node.Params )
                {
                    const Frontend::Param &Entry  = Ctx.Ast.GetParam( Id );
                    const SemaTypeId ParamType    = ResolveTypeExpr( Ctx.Ast, Ctx.Types, Generics(), Sink, Entry.DeclType );
                    LocalTypes[BindingSite{ Id }] = ParamType;
                    Locals[Entry.Name]            = ParamType;
                    Ctx.Values.SetSiteType( BindingSite{ Id }, ParamType );
                    InferExpr( Entry.Default );
                }
                WalkStmts( Node.Body );

                bStaticContext          = bOuterStatic;
                CurrentMethodReturnType = OuterReturnType;
                LocalTypes.swap( OuterLocalTypes );
                Locals.swap( OuterLocals );
                UnconstrainedLiterals.swap( OuterUnconstrainedLiterals );
                UnconstrainedVarInitializers.swap( OuterUnconstrainedVarInitializers );
            }

            void WalkDecl ( Frontend::DeclId Id )
            {
                if ( not Id.IsValid() )
                {
                    return;
                }

                std::visit(
                    Meta::Overloaded{
                        [&] ( const Frontend::Struct &Node )
                        {
                            EnterType( Ctx.Types.LookupType( Ctx.Ast.Text( Node.Name ) ).value_or( NominalId{} ), Node.Generics,
                                       Node.Body );
                        },
                        [&] ( const Frontend::Class &Node )
                        {
                            EnterType( Ctx.Types.LookupType( Ctx.Ast.Text( Node.Name ) ).value_or( NominalId{} ), Node.Generics,
                                       Node.Body );
                        },
                        [&] ( const Frontend::Mixin &Node )
                        {
                            EnterType( Ctx.Types.LookupType( Ctx.Ast.Text( Node.Name ) ).value_or( NominalId{} ), Node.Generics,
                                       Node.Body );
                        },
                        [&] ( const Frontend::Method &Node ) { EnterMethod( Node ); },
                        // Annotations are metadata; their arguments are never
                        // program values and are skipped wholesale.
                        [] ( const Frontend::Annotation & ) {},
                        // Everything else: walk whatever children reflection
                        // finds. A new declaration node is covered for free.
                        [&] ( const auto &Node ) { WalkChildren( Node ); },
                    },
                    Ctx.Ast.Decl( Id ) );
            }

            // --- Statements -----------------------------------------------

            void WalkStmts ( const Frontend::StmtList &Stmts )
            {
                for ( const Frontend::StmtId Id : Stmts )
                {
                    WalkStmt( Id );
                }
            }

            void WalkStmt ( Frontend::StmtId Id )
            {
                if ( not Id.IsValid() )
                {
                    return;
                }

                std::visit(
                    Meta::Overloaded{
                        [&] ( const Frontend::LocalDecl &Node )
                        {
                            UnitSink Sink{ .Values = Ctx.Values, .Self = SelfValue };
                            const SemaTypeId Written = ResolveTypeExpr( Ctx.Ast, Ctx.Types, Generics(), Sink, Node.DeclType );
                            if ( Written.IsValid() and Node.Init.IsValid() )
                            {
                                ConstrainExprType( Node.Init, Written );
                            }
                            const SemaTypeId Init  = InferExpr( Node.Init );
                            const SemaTypeId Bound = Written.IsValid() ? Written : Init;
                            // The declaration is its own binding site — the
                            // one ScopeResolver bound every in-scope use to.
                            LocalTypes[BindingSite{ Id }] = Bound;
                            Locals[Node.Name]             = Bound;
                            Ctx.Values.SetSiteType( BindingSite{ Id }, Bound );
                            if ( not Written.IsValid() and Node.Init.IsValid() )
                            {
                                UnconstrainedVarInitializers[Node.Name] = Node.Init;
                            }
                        },
                        [&] ( const Frontend::Return &Node )
                        {
                            if ( Node.Value.IsValid() and CurrentMethodReturnType.IsValid() )
                            {
                                ConstrainExprType( Node.Value, CurrentMethodReturnType );
                            }
                            WalkChildren( Node );
                        },
                        [&] ( const auto &Node ) { WalkChildren( Node ); },
                    },
                    Ctx.Ast.Stmt( Id ) );
            }

            // Reflection-driven default walk: recurse into every child node,
            // whatever category it belongs to. This is what keeps the pass
            // free of a per-node visitor.
            template <typename NodeType> void WalkChildren ( const NodeType &Node )
            {
                if constexpr ( Meta::Reflected<NodeType> )
                {
                    Meta::ForEachField( Node,
                                        [&] ( std::string_view, const auto &Field )
                                        {
                                            using FieldType = std::remove_cvref_t<decltype( Field )>;
                                            if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                                            {
                                                static_cast<void>( InferExpr( Field ) );
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
                                            {
                                                for ( const Frontend::ExprId Child : Field )
                                                {
                                                    static_cast<void>( InferExpr( Child ) );
                                                }
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, Frontend::StmtList> )
                                            {
                                                WalkStmts( Field );
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, Frontend::DeclList> )
                                            {
                                                WalkDecls( Field );
                                            }
                                            else if constexpr ( std::is_same_v<FieldType, Frontend::ParamList> )
                                            {
                                                for ( const Frontend::ParamId Child : Field )
                                                {
                                                    static_cast<void>( InferExpr( Ctx.Ast.GetParam( Child ).Default ) );
                                                }
                                            }
                                        } );
                }
            }

            // --- Member resolution ----------------------------------------

            // Look one member up on a receiver type and instantiate its
            // result. A member inherited from another type is instantiated
            // against *its* parameters, so a receiver's arguments only apply
            // when the receiver itself declares the member.
            [[nodiscard]] Resolution LookupOn ( SemaTypeId Receiver, std::string_view Name )
            {
                if ( not Ctx.Values.Has( Receiver ) )
                {
                    return Resolution{};
                }

                const NominalId Base             = Ctx.Values.Get( Receiver ).Base;
                const std::string_view CleanName = Name.starts_with( '@' ) ? Name.substr( 1 ) : Name;
                auto Found                       = Ctx.Types.LookupMember( Base, CleanName );
                if ( Found.Decl == nullptr and CleanName == "new" )
                {
                    Found = Ctx.Types.LookupMember( Base, "initialize" );
                }
                if ( Found.Decl == nullptr )
                {
                    return Resolution{};
                }

                // Copy before interning: Intern may grow the arena and
                // invalidate any reference into it.
                Core::SmallVec<SemaTypeId, 2> Args;
                if ( Found.Owner == Base )
                {
                    for ( const SemaTypeId Arg : Ctx.Values.Get( Receiver ).Args )
                    {
                        Args.PushBack( Arg );
                    }
                }

                const std::span<const SemaTypeId> Applied{ Args.begin(), Args.Size() };

                // `@[Apply]`: invoking a callable. Its signature is the
                // receiver's own arguments — result first, then parameters —
                // so the arity of `block.call( x )` follows the block, which
                // no written declaration could pin down.
                if ( Found.Decl->bApply )
                {
                    const Core::SmallVec<SemaTypeId, 2> &Signature = Ctx.Values.Get( Receiver ).Args;
                    Core::SmallVec<SemaTypeId, 4> Params;
                    for ( std::size_t Index = 1; Index < Signature.Size(); ++Index )
                    {
                        Params.PushBack( Signature[Index] );
                    }
                    return Resolution{ .Decl   = Found.Decl,
                                       .Result = Signature.IsEmpty() ? SemaTypeId{} : Signature[0],
                                       .Params = std::move( Params ) };
                }

                // A `&block` slot binds through the call's trailing block,
                // never a positional argument, so it is excluded here: the
                // result lines up one-to-one with Call::Args / DotCall::Args.
                Core::SmallVec<SemaTypeId, 4> Params;
                for ( std::size_t Index = 0; Index < Found.Decl->Params.Size(); ++Index )
                {
                    if ( Index < Found.Decl->ParamIsBlock.Size() and Found.Decl->ParamIsBlock[Index] )
                    {
                        continue;
                    }
                    Params.PushBack( Instantiate( Ctx.Types, Found.Decl->Params[Index], Applied, Receiver, Ctx.Values ) );
                }

                return Resolution{ .Decl   = Found.Decl,
                                   .Result = Instantiate( Ctx.Types, Found.Decl->Result, Applied, Receiver, Ctx.Values ),
                                   .Params = std::move( Params ) };
            }

            void CheckMemberSelf ( Core::SourceRange Loc, const Resolution &Found, bool bReceiverIsNakedType )
            {
                if ( Found.Decl == nullptr )
                {
                    return;
                }

                const bool bMemberIsStatic = Found.Decl->bSelf;
                const std::string Name     = std::string{ Ctx.Types.Text( Found.Decl->Name ) };

                if ( bReceiverIsNakedType and not bMemberIsStatic and Name != "initialize" )
                {
                    Report( Loc, "instance member " + Name + " cannot be accessed on a type" );
                }
                else if ( not bReceiverIsNakedType and bMemberIsStatic )
                {
                    Report( Loc, "static member " + Name + " cannot be accessed on an instance" );
                }
            }

            void CheckDotCallSelf ( Core::SourceRange Loc, const Resolution &Found )
            {
                if ( Found.Decl == nullptr )
                {
                    return;
                }

                const bool bMemberIsStatic = Found.Decl->bSelf;
                const std::string Name     = std::string{ Ctx.Types.Text( Found.Decl->Name ) };

                if ( bStaticContext and not bMemberIsStatic )
                {
                    Report( Loc, "instance member " + Name + " cannot be accessed from a static context" );
                }
                else if ( not bStaticContext and bMemberIsStatic )
                {
                    Report( Loc, "static member " + Name + " cannot be accessed from an instance context" );
                }
            }

            [[nodiscard]] static bool IsBuiltinPrimitiveOp ( std::string_view Name )
            {
                if ( Name.empty() )
                {
                    return false;
                }
                if ( Name == "and" or Name == "or" or Name == "not" )
                {
                    return true;
                }
                const char c = Name[0];
                return not( ( c >= 'a' and c <= 'z' ) or ( c >= 'A' and c <= 'Z' ) or c == '_' );
            }

            // A member access is an implicit call: `10.times` and
            // `"x".to_string` name the method and evaluate to what it
            // returns, exactly as they read.
            [[nodiscard]] SemaTypeId
            MemberType ( Core::SourceRange Loc, SemaTypeId Receiver, bool bReceiverIsNakedType, std::string_view Name )
            {
                const Resolution Found = LookupOn( Receiver, Name );
                if ( Ctx.Values.Has( Receiver ) and Found.Decl == nullptr )
                {
                    bool bIsBuiltinOp          = false;
                    const NominalId Base       = Ctx.Values.Get( Receiver ).Base;
                    const NominalType &Nominal = Ctx.Types.Type( Base );
                    if ( Nominal.Layout.IsValid() )
                    {
                        const LayoutKind Kind = KindOf( Ctx.Types.Get( Nominal.Layout ) );
                        if ( ( Kind == LayoutKind::Primitive or Kind == LayoutKind::Pointer ) and IsBuiltinPrimitiveOp( Name ) )
                        {
                            bIsBuiltinOp = true;
                        }
                    }

                    if ( not bIsBuiltinOp )
                    {
                        Report( Loc, "type " + NameOfValue( Receiver ) + " has no member '" + std::string( Name ) + "'" );
                    }
                }
                CheckMemberSelf( Loc, Found, bReceiverIsNakedType );
                return Found.Result;
            }

            // --- Closures ---------------------------------------------------

            // Bind a closure's parameters as locals and report their types in
            // order. Shared by `( x ) => …` and `do | x | … end`, which differ
            // only in how their body is spelled.
            [[nodiscard]] Core::SmallVec<SemaTypeId, 2> BindClosureParams ( const Frontend::ParamList &Params )
            {
                UnitSink Sink{ .Values = Ctx.Values, .Self = SelfValue };
                Core::SmallVec<SemaTypeId, 2> Types;
                for ( const Frontend::ParamId PId : Params )
                {
                    const Frontend::Param &Entry   = Ctx.Ast.GetParam( PId );
                    const SemaTypeId ParamType     = ResolveTypeExpr( Ctx.Ast, Ctx.Types, Generics(), Sink, Entry.DeclType );
                    LocalTypes[BindingSite{ PId }] = ParamType;
                    Locals[Entry.Name]             = ParamType;
                    Ctx.Values.SetSiteType( BindingSite{ PId }, ParamType );
                    if ( Entry.Default.IsValid() )
                    {
                        static_cast<void>( InferExpr( Entry.Default ) );
                    }
                    Types.PushBack( ParamType );
                }
                return Types;
            }

            // A statement body evaluates to its trailing expression, as it
            // reads. Every statement is still walked — a block's body is
            // checked whether or not its value is used.
            [[nodiscard]] SemaTypeId TrailingType ( const Frontend::StmtList &Body )
            {
                SemaTypeId Last;
                for ( const Frontend::StmtId Id : Body )
                {
                    WalkStmt( Id );
                    Last = SemaTypeId{};
                    if ( const auto *Stmt = std::get_if<Frontend::ExprStmt>( &Ctx.Ast.Stmt( Id ) ) )
                    {
                        Last = Ctx.Values.ExprType( Stmt->Expr );
                    }
                }
                return Last;
            }

            // A closure's type is the stdlib type claiming `NodeKind`, applied
            // to its result then its parameters — the argument order the
            // FuncType node reflects, so a written `T -> U` and the block that
            // satisfies it produce the very same type.
            //
            // Captures do not appear: an environment is a memory concern that
            // ClosureFrame derives from the ScopeTable, not part of what a
            // callable *is*.
            [[nodiscard]] SemaTypeId
            ClosureType ( std::string_view NodeKind, SemaTypeId Result, const Core::SmallVec<SemaTypeId, 2> &Params )
            {
                const auto Base = Ctx.Types.LookupNodeKind( NodeKind );
                if ( not Base )
                {
                    return SemaTypeId{};
                }

                Core::SmallVec<SemaTypeId, 2> Args;
                Args.PushBack( Result );
                for ( const SemaTypeId Param : Params )
                {
                    Args.PushBack( Param );
                }
                return MakeType( *Base, std::move( Args ) );
            }

            // --- Expressions ------------------------------------------------

            SemaTypeId InferExpr ( Frontend::ExprId Id )
            {
                if ( not Id.IsValid() or ( Id.Value < Metadata.size() and Metadata[Id.Value] ) )
                {
                    return SemaTypeId{};
                }
                if ( const SemaTypeId Known = Ctx.Values.ExprType( Id ); Known.IsValid() )
                {
                    return Known;
                }

                const SemaTypeId Type = Compute( Id );
                Ctx.Values.SetExprType( Id, Type );
                return Type;
            }

            SemaTypeId Compute ( Frontend::ExprId Id )
            {
                const Frontend::ExprNode &Node = Ctx.Ast.Expr( Id );

                return std::visit(
                    Meta::Overloaded{
                        [&] ( const Frontend::SelfExpr & ) -> SemaTypeId { return SelfValue; },
                        [&] ( const Frontend::InstanceVar &Expr ) -> SemaTypeId
                        {
                            return MemberType( Frontend::LocOf( Ctx.Ast.Expr( Id ) ), SelfValue, bStaticContext,
                                               Ctx.Ast.Text( Expr.Name ) );
                        },
                        [&] ( const Frontend::Identifier &Expr ) -> SemaTypeId
                        {
                            if ( const std::optional<SemaTypeId> Local = FindLocal( Id, Expr.Name ) )
                            {
                                return *Local;
                            }
                            // A bare name may also be a type used as a value
                            // (`Pointer.malloc`); the receiver is then the
                            // type itself, un-instantiated.
                            if ( const auto Named = Ctx.Types.LookupType( Ctx.Ast.Text( Expr.Name ) ) )
                            {
                                NakedTypeExprs.insert( Id.Value );
                                return MakeType( *Named, {} );
                            }
                            return SemaTypeId{};
                        },
                        [&] ( const Frontend::Member &Expr ) -> SemaTypeId
                        {
                            const SemaTypeId Object    = InferExpr( Expr.Object );
                            const Resolution Found     = LookupOn( Object, Ctx.Ast.Text( Expr.Name ) );
                            CalleeResolution[Id.Value] = Found;
                            if ( Ctx.Values.Has( Object ) and Found.Decl == nullptr )
                            {
                                Report( Frontend::LocOf( Ctx.Ast.Expr( Id ) ),
                                        "type " + NameOfValue( Object ) + " has no member '" +
                                            std::string{ Ctx.Ast.Text( Expr.Name ) } + "'" );
                            }
                            CheckMemberSelf( Frontend::LocOf( Ctx.Ast.Expr( Id ) ), Found,
                                             NakedTypeExprs.contains( Expr.Object.Value ) );
                            return Found.Result;
                        },
                        [&] ( const Frontend::Index &Expr ) -> SemaTypeId
                        {
                            const SemaTypeId Object = InferExpr( Expr.Object );
                            for ( const Frontend::ExprId Arg : Expr.Args )
                            {
                                static_cast<void>( InferExpr( Arg ) );
                            }
                            return MemberType( Frontend::LocOf( Ctx.Ast.Expr( Id ) ), Object,
                                               NakedTypeExprs.contains( Expr.Object.Value ), IndexOperator );
                        },
                        // An operator is a method: the spelling comes from the
                        // token (an AST property), the method from source/Lib/.
                        [&] ( const Frontend::Binary &Expr ) -> SemaTypeId
                        {
                            const SemaTypeId Lhs = InferExpr( Expr.Lhs );
                            const SemaTypeId Rhs = InferExpr( Expr.Rhs );
                            if ( Lhs.IsValid() and not UnconstrainedLiterals.contains( Expr.Lhs.Value ) )
                            {
                                ConstrainExprType( Expr.Rhs, Lhs );
                            }
                            else if ( Rhs.IsValid() and not UnconstrainedLiterals.contains( Expr.Rhs.Value ) )
                            {
                                ConstrainExprType( Expr.Lhs, Rhs );
                            }
                            return MemberType( Frontend::LocOf( Ctx.Ast.Expr( Id ) ), InferExpr( Expr.Lhs ),
                                               NakedTypeExprs.contains( Expr.Lhs.Value ), Frontend::TokenSpelling( Expr.Op ) );
                        },
                        [&] ( const Frontend::Unary &Expr ) -> SemaTypeId
                        {
                            return MemberType( Frontend::LocOf( Ctx.Ast.Expr( Id ) ), InferExpr( Expr.Operand ),
                                               NakedTypeExprs.contains( Expr.Operand.Value ),
                                               Frontend::TokenSpelling( Expr.Op ) );
                        },
                        [&] ( const Frontend::Assign &Expr ) -> SemaTypeId
                        {
                            const SemaTypeId Value = InferExpr( Expr.Value );
                            if ( const auto *Target = std::get_if<Frontend::Identifier>( &Ctx.Ast.Expr( Expr.Target ) ) )
                            {
                                const std::optional<SemaTypeId> Known = FindLocal( Expr.Target, Target->Name );
                                if ( Known.has_value() and Known->IsValid() )
                                {
                                    ConstrainExprType( Expr.Value, *Known );
                                }
                                else
                                {
                                    WriteLocal( Expr.Target, Target->Name, Value );
                                    UnconstrainedVarInitializers[Target->Name] = Expr.Value;
                                }
                            }
                            static_cast<void>( InferExpr( Expr.Target ) );
                            return Value;
                        },
                        [&] ( const Frontend::Ternary &Expr ) -> SemaTypeId
                        {
                            static_cast<void>( InferExpr( Expr.Cond ) );
                            const SemaTypeId Then = InferExpr( Expr.Then );
                            const SemaTypeId Else = InferExpr( Expr.Else );
                            return Then.IsValid() ? Then : Else;
                        },
                        [&] ( const Frontend::Call &Expr ) -> SemaTypeId { return CallType( Expr ); },
                        [&] ( const Frontend::GenericInst &Expr ) -> SemaTypeId { return GenericInstType( Id, Expr ); },
                        // `.method(args)` shorthand: the same call as
                        // `self.method(args)` — CaseLowering (order 22)
                        // already rewrites the ones it finds inside a `when`
                        // pattern, so any that reach here name a method on
                        // the enclosing type directly.
                        [&] ( const Frontend::DotCall &Expr ) -> SemaTypeId
                        {
                            for ( const Frontend::ExprId Arg : Expr.Args )
                            {
                                static_cast<void>( InferExpr( Arg ) );
                            }
                            const Resolution Found = LookupOn( SelfValue, Ctx.Ast.Text( Expr.Method ) );
                            if ( Ctx.Values.Has( SelfValue ) and Found.Decl == nullptr )
                            {
                                Report( Frontend::LocOf( Ctx.Ast.Expr( Id ) ),
                                        "type " + NameOfValue( SelfValue ) + " has no member '" +
                                            std::string{ Ctx.Ast.Text( Expr.Method ) } + "'" );
                            }
                            CheckDotCallSelf( Frontend::LocOf( Ctx.Ast.Expr( Id ) ), Found );
                            CheckCallArgs( Expr.Loc, Found, Expr.Args );
                            return Found.Result;
                        },
                        [&] ( const Frontend::Lambda &Expr ) -> SemaTypeId
                        {
                            const Core::SmallVec<SemaTypeId, 2> ParamTypes = BindClosureParams( Expr.Params );
                            if ( Expr.ReturnType.IsValid() )
                            {
                                const SemaTypeId WrittenRet = InferExpr( Expr.ReturnType );
                                if ( WrittenRet.IsValid() )
                                {
                                    ConstrainExprType( Expr.Body, WrittenRet );
                                }
                            }
                            return ClosureType( "Lambda", InferExpr( Expr.Body ), ParamTypes );
                        },
                        [&] ( const Frontend::Block &Expr ) -> SemaTypeId
                        {
                            const Core::SmallVec<SemaTypeId, 2> ParamTypes = BindClosureParams( Expr.Params );
                            return ClosureType( "Block", TrailingType( Expr.Body ), ParamTypes );
                        },
                        [&] ( const auto &Expr ) -> SemaTypeId { return LiteralType( Id, Expr ); },
                    },
                    Node );
            }

            [[nodiscard]] SemaTypeId CallType ( const Frontend::Call &Expr )
            {
                for ( const Frontend::ExprId Arg : Expr.Args )
                {
                    static_cast<void>( InferExpr( Arg ) );
                }
                static_cast<void>( InferExpr( Expr.BlockArg ) );

                // The callee already resolved to the member's result: a call
                // and a bare member access denote the same thing, so the type
                // of the call is the type of its callee.
                const SemaTypeId Result = InferExpr( Expr.Callee );

                // When the callee is a `receiver.name` that resolved to a
                // method, its Member::Params is the signature this call must
                // satisfy — arity and per-argument types alike.
                if ( const auto It = CalleeResolution.find( Expr.Callee.Value ); It != CalleeResolution.end() )
                {
                    CheckCallArgs( Expr.Loc, It->second, Expr.Args );
                }

                return Result;
            }

            // Arity and per-argument type checking against a resolved
            // method's already-instantiated Params. Skipped for a field (a
            // field's *value* being called is not a call this file resolves)
            // and for anything that failed to resolve at all — neither is a
            // new diagnostic invented here.
            void CheckCallArgs ( Core::SourceRange Loc, const Resolution &Found, const Frontend::ExprList &Args )
            {
                if ( Found.Decl == nullptr or Found.Decl->Kind != EMemberKind::Method )
                {
                    return;
                }

                const std::string Name = std::string{ Ctx.Types.Text( Found.Decl->Name ) };

                const std::size_t Expected = Found.Params.Size();
                const std::size_t Given    = Args.Size();
                if ( Given != Expected )
                {
                    Report( Loc, Name + " takes " + std::to_string( Expected ) + " argument(s), but " + std::to_string( Given ) +
                                     " were given" );
                    return;
                }

                for ( std::size_t Index = 0; Index < Given; ++Index )
                {
                    const SemaTypeId ParamType = Found.Params[Index];
                    if ( ParamType.IsValid() )
                    {
                        ConstrainExprType( Args[Index], ParamType );
                    }
                    const SemaTypeId ArgType = InferExpr( Args[Index] );
                    if ( ArgType.IsValid() and ParamType.IsValid() and ArgType.Value != ParamType.Value )
                    {
                        Report( Frontend::LocOf( Ctx.Ast.Expr( Args[Index] ) ),
                                "argument " + std::to_string( Index + 1 ) + " to " + Name + " has type " +
                                    NameOfValue( ArgType ) + ", expected " + NameOfValue( ParamType ) );
                    }
                }
            }

            [[nodiscard]] SemaTypeId GenericInstType ( Frontend::ExprId Id, const Frontend::GenericInst &Expr )
            {
                const SemaTypeId Base = InferExpr( Expr.Base );
                if ( NakedTypeExprs.contains( Expr.Base.Value ) )
                {
                    NakedTypeExprs.insert( Id.Value );
                }
                if ( not Ctx.Values.Has( Base ) )
                {
                    return SemaTypeId{};
                }
                const NominalId Nominal = Ctx.Values.Get( Base ).Base;

                Core::SmallVec<SemaTypeId, 2> Args;
                for ( const Frontend::TypeId Arg : Expr.Args )
                {
                    UnitSink Sink{ .Values = Ctx.Values, .Self = SelfValue };
                    Args.PushBack( ResolveTypeExpr( Ctx.Ast, Ctx.Types, Generics(), Sink, Arg ) );
                }

                CheckArity( Expr.Loc, Nominal, Args.Size() );
                return MakeType( Nominal, std::move( Args ) );
            }

            void CheckArity ( Core::SourceRange Loc, NominalId Base, std::size_t Given )
            {
                if ( not Base.IsValid() )
                {
                    return;
                }
                const std::size_t Expected = Ctx.Types.Type( Base ).Params.Size();
                if ( Given != Expected )
                {
                    Report( Loc, NameOf( Base ) + " takes " + std::to_string( Expected ) + " type argument(s), but " +
                                     std::to_string( Given ) + " were given" );
                }
            }

            // --- The one literal rule --------------------------------------

            // A node is a literal *because a Volt type claimed its kind*, not
            // because this file knows the node. The generic arguments come, by
            // default, from the node's expression-bearing fields in
            // declaration order — one slot per field, an ExprList joining its
            // elements. `@[Literal( Kind, A, [ B, C ] )]` overrides that by
            // naming the fields feeding each parameter.
            template <typename NodeType> [[nodiscard]] SemaTypeId LiteralType ( Frontend::ExprId Id, const NodeType &Node )
            {
                WalkChildren( Node );

                if constexpr ( not Meta::Reflected<NodeType> )
                {
                    return SemaTypeId{};
                }
                else
                {
                    const std::string_view Kind = Meta::TypeName<NodeType>();
                    if ( Kind == "IntLiteral" )
                    {
                        UnconstrainedLiterals.insert( Id.Value );
                    }
                    const auto Base = Ctx.Types.LookupNodeKind( Kind );
                    if ( not Base )
                    {
                        // A written value with no type to wrap it means the
                        // stdlib never claimed that kind — a missing
                        // `@[Literal]`, not a user error in this file.
                        if constexpr ( Meta::FieldCount<NodeType>() > 0 and not HasChildNodes<NodeType>() )
                        {
                            Report( Frontend::LocOf( Ctx.Ast.Expr( Id ) ),
                                    "no type claims " + std::string{ Kind } +
                                        "; the standard library must declare one with @[Literal( " + std::string{ Kind } +
                                        " )]" );
                        }
                        return SemaTypeId{};
                    }

                    Core::SmallVec<SemaTypeId, 2> Args = SlotTypes( *Base, Node );
                    CheckArity( Frontend::LocOf( Ctx.Ast.Expr( Id ) ), *Base, Args.Size() );
                    return MakeType( *Base, std::move( Args ) );
                }
            }

            // The type feeding one generic parameter: an ExprId field is its
            // own type, an ExprList field the join of its elements (the first
            // resolved one — an empty literal simply leaves it unresolved).
            template <typename FieldType> void JoinField ( SemaTypeId &Slot, const FieldType &Field )
            {
                if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> )
                {
                    if ( not Slot.IsValid() )
                    {
                        Slot = InferExpr( Field );
                    }
                }
                else if constexpr ( std::is_same_v<FieldType, Frontend::ExprList> )
                {
                    for ( const Frontend::ExprId Child : Field )
                    {
                        if ( not Slot.IsValid() )
                        {
                            Slot = InferExpr( Child );
                        }
                    }
                }
            }

            template <typename NodeType>
            [[nodiscard]] Core::SmallVec<SemaTypeId, 2> SlotTypes ( NominalId Base, const NodeType &Node )
            {
                Core::SmallVec<SemaTypeId, 2> Args;
                const auto &Slots = Ctx.Types.Type( Base ).LiteralSlots;

                if ( Slots.empty() )
                {
                    Meta::ForEachField( Node,
                                        [&] ( std::string_view, const auto &Field )
                                        {
                                            using FieldType = std::remove_cvref_t<decltype( Field )>;
                                            if constexpr ( std::is_same_v<FieldType, Frontend::ExprId> or
                                                           std::is_same_v<FieldType, Frontend::ExprList> )
                                            {
                                                SemaTypeId Slot;
                                                JoinField( Slot, Field );
                                                Args.PushBack( Slot );
                                            }
                                        } );
                    return Args;
                }

                for ( const auto &Names : Slots )
                {
                    SemaTypeId Slot;
                    Meta::ForEachField( Node,
                                        [&] ( std::string_view FieldName, const auto &Field )
                                        {
                                            for ( const Symbol Wanted : Names )
                                            {
                                                if ( Ctx.Types.Text( Wanted ) == FieldName )
                                                {
                                                    JoinField( Slot, Field );
                                                }
                                            }
                                        } );
                    Args.PushBack( Slot );
                }
                return Args;
            }
        };

    } // namespace

    void TypeChecker ( PassContext &Context )
    {
        Checker Walk{ Context, MetadataExprs( Context.Ast ) };
        Walk.WalkDecls( Context.Ast.TopDecls );
        for ( const Frontend::StmtId Id : Context.Ast.TopStmts )
        {
            Walk.WalkStmt( Id );
        }
    }

} // namespace Sema

} // namespace Volt
