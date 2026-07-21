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

#include <meta>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
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
        // result type.
        struct Resolution
        {

            const Member *Decl = nullptr;
            SemaTypeId Result;
        };

        struct Checker
        {

            PassContext &Ctx;
            std::vector<bool> Metadata;

            // The type whose body we are inside, and the AST symbols of its
            // generic parameters (matched by ResolveTypeExpr, which reads
            // written names out of *this* unit's interner).
            NominalId SelfType{};
            const Frontend::SymbolList *SelfGenerics = nullptr;
            SemaTypeId SelfValue{};

            // Locals of the method body being walked. Scope resolution proper
            // is ScopeResolver's job; until it publishes a table, a flat
            // per-body map is enough to type what the stdlib writes.
            std::unordered_map<Symbol, SemaTypeId> Locals{};

            void Report ( Core::SourceRange Loc, std::string Message )
            {
                Ctx.Diags.Report( Core::Diagnostic{
                    .Severity = Core::ESeverity::Error, .Range = Loc, .Message = std::move( Message ), .Notes = {} } );
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
                std::unordered_map<Symbol, SemaTypeId> Outer;
                Outer.swap( Locals );

                for ( const Frontend::ParamId Id : Node.Params )
                {
                    const Frontend::Param &Entry = Ctx.Ast.GetParam( Id );
                    UnitSink Sink{ Ctx.Values };
                    Locals[Entry.Name] = ResolveTypeExpr( Ctx.Ast, Ctx.Types, Generics(), Sink, Entry.DeclType );
                    InferExpr( Entry.Default );
                }
                WalkStmts( Node.Body );

                Locals.swap( Outer );
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
                            const SemaTypeId Init = InferExpr( Node.Init );
                            UnitSink Sink{ Ctx.Values };
                            const SemaTypeId Written = ResolveTypeExpr( Ctx.Ast, Ctx.Types, Generics(), Sink, Node.DeclType );
                            Locals[Node.Name]        = Written.IsValid() ? Written : Init;
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

                const NominalId Base = Ctx.Values.Get( Receiver ).Base;
                const auto Found     = Ctx.Types.LookupMember( Base, Name );
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
                return Resolution{ .Decl   = Found.Decl,
                                   .Result = Instantiate( Ctx.Types, Found.Decl->Result, Applied, Ctx.Values ) };
            }

            // A member access is an implicit call: `10.times` and
            // `"x".to_string` name the method and evaluate to what it
            // returns, exactly as they read.
            [[nodiscard]] SemaTypeId MemberType ( SemaTypeId Receiver, std::string_view Name )
            {
                return LookupOn( Receiver, Name ).Result;
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
                        { return MemberType( SelfValue, Ctx.Ast.Text( Expr.Name ) ); },
                        [&] ( const Frontend::Identifier &Expr ) -> SemaTypeId
                        {
                            if ( const auto It = Locals.find( Expr.Name ); It != Locals.end() )
                            {
                                return It->second;
                            }
                            // A bare name may also be a type used as a value
                            // (`Pointer.malloc`); the receiver is then the
                            // type itself, un-instantiated.
                            if ( const auto Named = Ctx.Types.LookupType( Ctx.Ast.Text( Expr.Name ) ) )
                            {
                                return MakeType( *Named, {} );
                            }
                            return SemaTypeId{};
                        },
                        [&] ( const Frontend::Member &Expr ) -> SemaTypeId
                        { return MemberType( InferExpr( Expr.Object ), Ctx.Ast.Text( Expr.Name ) ); },
                        [&] ( const Frontend::Index &Expr ) -> SemaTypeId
                        {
                            const SemaTypeId Object = InferExpr( Expr.Object );
                            for ( const Frontend::ExprId Arg : Expr.Args )
                            {
                                static_cast<void>( InferExpr( Arg ) );
                            }
                            return MemberType( Object, IndexOperator );
                        },
                        // An operator is a method: the spelling comes from the
                        // token (an AST property), the method from source/Lib/.
                        [&] ( const Frontend::Binary &Expr ) -> SemaTypeId
                        {
                            const SemaTypeId Lhs = InferExpr( Expr.Lhs );
                            static_cast<void>( InferExpr( Expr.Rhs ) );
                            return MemberType( Lhs, Frontend::TokenSpelling( Expr.Op ) );
                        },
                        [&] ( const Frontend::Unary &Expr ) -> SemaTypeId
                        { return MemberType( InferExpr( Expr.Operand ), Frontend::TokenSpelling( Expr.Op ) ); },
                        [&] ( const Frontend::Assign &Expr ) -> SemaTypeId
                        {
                            const SemaTypeId Value = InferExpr( Expr.Value );
                            // `x = expr` on a not-yet-known name introduces it.
                            if ( const auto *Target = std::get_if<Frontend::Identifier>( &Ctx.Ast.Expr( Expr.Target ) );
                                 Target != nullptr and not Locals.contains( Target->Name ) )
                            {
                                Locals[Target->Name] = Value;
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
                        [&] ( const Frontend::GenericInst &Expr ) -> SemaTypeId { return GenericInstType( Expr ); },
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
                return InferExpr( Expr.Callee );
            }

            [[nodiscard]] SemaTypeId GenericInstType ( const Frontend::GenericInst &Expr )
            {
                const SemaTypeId Base = InferExpr( Expr.Base );
                if ( not Ctx.Values.Has( Base ) )
                {
                    return SemaTypeId{};
                }
                const NominalId Nominal = Ctx.Values.Get( Base ).Base;

                Core::SmallVec<SemaTypeId, 2> Args;
                for ( const Frontend::TypeId Arg : Expr.Args )
                {
                    UnitSink Sink{ Ctx.Values };
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
                    const auto Base             = Ctx.Types.LookupNodeKind( Kind );
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
