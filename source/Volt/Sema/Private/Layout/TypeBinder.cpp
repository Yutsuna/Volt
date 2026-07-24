// TypeBinder.cpp — the serial cross-unit seam that turns `@[Primitive(...)]`
// and `@[Literal(...)]` into TypeStore bindings, and publishes each type's
// interface (generic parameters, members, super, mixins).
//
// Zero-hardcode lives or dies here: this file must never mention `Int32`,
// `String` or any other Volt type. It reads the *spelling* out of the
// annotation ("i32") and the node kind the type claims ("IntLiteral"), and
// binds the two. Which Volt type wraps a bare `10` is decided entirely by
// which struct carries `@[Literal( IntLiteral )]` in source/Lib/.
//
// It runs in two phases, both serial: declaring every name (phase A) must
// complete across all units before any signature is resolved (phase B), or a
// member's return type would depend on stdlib file order.

#include "Volt/Sema/Layout/TypeBinder.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Type.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

#include <charconv>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Volt
{

namespace Sema
{

    namespace
    {

        struct PendingAnnotation
        {

            Symbol Name;
            Frontend::ExprList Args;
            Core::SourceRange Loc;
        };

        // What both phases need out of a declaration that introduces a type.
        struct TypeDecl
        {

            std::string_view Name;
            Frontend::DeclId Id;
            const Frontend::SymbolList *Generics = nullptr;
            const Frontend::DeclList *Body       = nullptr;
            Frontend::TypeId Super;
        };

        // Walk the declarations of one scope, invoking Visit for every
        // declaration that introduces a named type, and recursing into
        // modules. Annotations accumulate onto the declaration they precede.
        // Shared by both phases so the two can never drift apart.
        template <typename DeclContainer, typename Fn>
        void ForEachTypeDecl ( const Frontend::AstContext &Ast, const DeclContainer &Decls, Fn &&Visit )
        {
            std::vector<PendingAnnotation> Pending;

            for ( const Frontend::DeclId Id : Decls )
            {
                if ( not Id.IsValid() )
                {
                    continue;
                }

                const Frontend::DeclNode &Node = Ast.Decl( Id );

                if ( const auto *Anno = std::get_if<Frontend::Annotation>( &Node ) )
                {
                    Pending.push_back( PendingAnnotation{ .Name = Anno->Name, .Args = Anno->Args, .Loc = Anno->Loc } );
                    continue;
                }

                // One overload per declaration that introduces a type;
                // everything else falls through the catch-all. No switch over
                // DeclKind, so a new category cannot desync this.
                std::visit(
                    Meta::Overloaded{
                        [&] ( const Frontend::Module &Nested ) { ForEachTypeDecl( Ast, Nested.Body, Visit ); },
                        [&] ( const Frontend::Struct &Type )
                        {
                            Visit( TypeDecl{ .Name     = Ast.Text( Type.Name ),
                                             .Id       = Id,
                                             .Generics = &Type.Generics,
                                             .Body     = &Type.Body,
                                             .Super    = Frontend::TypeId{} },
                                   Pending );
                        },
                        [&] ( const Frontend::Class &Type )
                        {
                            Visit( TypeDecl{ .Name     = Ast.Text( Type.Name ),
                                             .Id       = Id,
                                             .Generics = &Type.Generics,
                                             .Body     = &Type.Body,
                                             .Super    = Type.Super },
                                   Pending );
                        },
                        [&] ( const Frontend::Mixin &Type )
                        {
                            Visit( TypeDecl{ .Name     = Ast.Text( Type.Name ),
                                             .Id       = Id,
                                             .Generics = &Type.Generics,
                                             .Body     = &Type.Body,
                                             .Super    = Frontend::TypeId{} },
                                   Pending );
                        },
                        [&] ( const Frontend::Enum &Type )
                        {
                            Visit( TypeDecl{ .Name     = Ast.Text( Type.Name ),
                                             .Id       = Id,
                                             .Generics = &Type.Generics,
                                             .Body     = &Type.Body,
                                             .Super    = Frontend::TypeId{} },
                                   Pending );
                        },
                        [] ( const auto & ) {},
                    },
                    Node );

                // Annotations bind to the declaration they precede, and never
                // carry past it.
                Pending.clear();
            }
        }

        // Walk the declarations of one scope, invoking Visit for every `def`
        // declared directly in a module — never one nested inside a
        // Class/Struct/Mixin/Component body, which DeclareMembers already
        // owns. Recurses into nested modules only, mirroring ForEachTypeDecl,
        // so the two traversals can never desync on what counts as
        // "top-level".
        template <typename DeclContainer, typename Fn>
        void ForEachFreeFunction ( const Frontend::AstContext &Ast, const DeclContainer &Decls, Fn &&Visit )
        {
            for ( const Frontend::DeclId Id : Decls )
            {
                if ( not Id.IsValid() )
                {
                    continue;
                }

                std::visit(
                    Meta::Overloaded{
                        [&] ( const Frontend::Module &Nested ) { ForEachFreeFunction( Ast, Nested.Body, Visit ); },
                        [&] ( const Frontend::Method &Entry ) { Visit( Id, Entry ); },
                        [] ( const auto & ) {},
                    },
                    Ast.Decl( Id ) );
            }
        }

        // The bit width of `@[Primitive( "i32", 32 )]`'s second argument.
        // Absent or malformed means "width unknown" (0), which is legal: a
        // pointer-shaped primitive may leave it to the target layout.
        [[nodiscard]] std::uint32_t ReadBits ( const Frontend::AstContext &Ast, Frontend::ExprId Id )
        {
            const auto *Literal = std::get_if<Frontend::IntLiteral>( &Ast.Expr( Id ) );
            if ( Literal == nullptr )
            {
                return 0;
            }

            const std::string_view Raw = Ast.Text( Literal->Raw );
            std::uint32_t Bits         = 0;
            // from_chars over the interned view: Text() is not NUL-terminated,
            // so strtoul would read past the end of the string block.
            std::from_chars( Raw.data(), Raw.data() + Raw.size(), Bits );
            return Bits;
        }

        // The layout of a type annotation's field, when the field's type name
        // is already bound. An unresolved field keeps an invalid LayoutId
        // rather than inventing one — full resolution is the TypeChecker's job.
        [[nodiscard]] LayoutId FieldLayoutOf ( const Frontend::AstContext &Ast, const TypeStore &Store, Frontend::TypeId Id )
        {
            if ( not Id.IsValid() )
            {
                return LayoutId{};
            }
            const auto *Ref = std::get_if<Frontend::TypeRef>( &Ast.Type( Id ) );
            if ( Ref == nullptr or Ref->Path.Size() == 0 )
            {
                return LayoutId{};
            }
            const Symbol Last = Ref->Path[Ref->Path.Size() - 1];
            if ( const auto Found = Store.LookupType( Ast.Text( Last ) ) )
            {
                return Store.Type( *Found ).Layout;
            }
            return LayoutId{};
        }

        // --- Phase A ---------------------------------------------------------

        struct Binder
        {

            const Frontend::AstContext &Ast;
            TypeStore &Store;
            Core::DiagEngine::Bag &Diags;
            std::uint32_t Unit = 0;
            std::size_t Bound  = 0;

            void Report ( Core::ESeverity Severity, Core::SourceRange Loc, const std::string &Message )
            {
                Diags.Report( Core::Diagnostic{ .Severity = Severity, .Range = Loc, .Message = Message, .Notes = {} } );
            }

            // The aggregate a struct collapses to when it has no
            // `@[Primitive]`: its fields in declaration order.
            [[nodiscard]] LayoutId AggregateOf ( const Frontend::DeclList &Body )
            {
                Aggregate Agg;
                for ( const Frontend::DeclId Id : Body )
                {
                    if ( const auto *Field = std::get_if<Frontend::Field>( &Ast.Decl( Id ) ) )
                    {
                        Agg.Fields.PushBack( FieldLayout{ .Name = Store.Intern( Ast.Text( Field->Name ) ),
                                                          .Type = FieldLayoutOf( Ast, Store, Field->DeclType ) } );
                    }
                }
                return Store.AddAggregate( std::move( Agg ) );
            }

            // `Id` instantiated with its own generic parameters, in order —
            // what `self` means inside `Id`'s own body. This is the result
            // type of every enum case: `Optional<T>::Some`/`::None` both
            // produce an `Optional<T>`, not a bare `Optional`.
            [[nodiscard]] SigTypeId SelfSigOf ( NominalId Id )
            {
                SigType Self;
                Self.Base = Id;
                for ( std::size_t Index = 0; Index < Store.Type( Id ).Params.Size(); ++Index )
                {
                    SigType Param;
                    Param.ParamIndex = static_cast<std::int32_t>( Index );
                    Self.Args.PushBack( Store.AddSig( std::move( Param ) ) );
                }
                return Store.AddSig( std::move( Self ) );
            }

            // Every field, method and enum case of the body becomes a
            // member, with its signature still unresolved — phase B fills
            // those in. (An enum case's Result is the one exception: it is
            // already fully known here, since it never depends on anything
            // but the enum's own identity.)
            void DeclareMembers ( NominalId Id, const Frontend::DeclList &Body )
            {
                bool bApply = false;

                for ( const Frontend::DeclId Child : Body )
                {
                    if ( not Child.IsValid() )
                    {
                        continue;
                    }

                    // Annotations bind to the member they precede, exactly as
                    // they do at file scope.
                    if ( const auto *Anno = std::get_if<Frontend::Annotation>( &Ast.Decl( Child ) ) )
                    {
                        bApply = bApply or Ast.Text( Anno->Name ) == "Apply";
                        continue;
                    }

                    std::visit(
                        Meta::Overloaded{
                            [&] ( const Frontend::Field &Entry )
                            {
                                Member Slot;
                                Slot.Name = Store.Intern( Ast.Text( Entry.Name ) );
                                Slot.Kind = EMemberKind::Field;
                                Slot.Unit = Unit;
                                Slot.Decl = Child;
                                Store.AddMember( Id, std::move( Slot ) );
                            },
                            [&] ( const Frontend::Method &Entry )
                            {
                                Member Slot;
                                Slot.Name      = Store.Intern( Ast.Text( Entry.Name ) );
                                Slot.Kind      = EMemberKind::Method;
                                Slot.Unit      = Unit;
                                Slot.Decl      = Child;
                                Slot.bSelf     = Entry.bSelf;
                                Slot.bApply    = bApply;
                                Slot.bAbstract = Entry.bAbstract;
                                Store.AddMember( Id, std::move( Slot ) );
                            },
                            [&] ( const Frontend::EnumCase &Entry )
                            {
                                Member Slot;
                                Slot.Name   = Store.Intern( Ast.Text( Entry.Name ) );
                                Slot.Kind   = EMemberKind::EnumCase;
                                Slot.Unit   = Unit;
                                Slot.Decl   = Child;
                                Slot.Result = SelfSigOf( Id );
                                Store.AddMember( Id, std::move( Slot ) );
                            },
                            [] ( const auto & ) {},
                        },
                        Ast.Decl( Child ) );

                    bApply = false;
                }
            }

            // The extra arguments of `@[Literal( Kind, A, [ B, C ] )]`: one
            // entry per generic parameter, naming the AST field(s) that feed
            // it. `[ B, C ]` joins two fields onto one parameter. Empty means
            // "use the default convention".
            [[nodiscard]] std::vector<Core::SmallVec<Symbol, 2>> ReadLiteralSlots ( const Frontend::ExprList &Args )
            {
                std::vector<Core::SmallVec<Symbol, 2>> Slots;
                for ( std::size_t Index = 1; Index < Args.Size(); ++Index )
                {
                    Core::SmallVec<Symbol, 2> Slot;
                    const Frontend::ExprNode &Arg = Ast.Expr( Args[static_cast<std::uint32_t>( Index )] );
                    std::visit(
                        Meta::Overloaded{
                            [&] ( const Frontend::Identifier &Name ) { Slot.PushBack( Store.Intern( Ast.Text( Name.Name ) ) ); },
                            [&] ( const Frontend::ArrayLit &Group )
                            {
                                for ( const Frontend::ExprId Element : Group.Elements )
                                {
                                    if ( const auto *Name = std::get_if<Frontend::Identifier>( &Ast.Expr( Element ) ) )
                                    {
                                        Slot.PushBack( Store.Intern( Ast.Text( Name->Name ) ) );
                                    }
                                }
                            },
                            [] ( const auto & ) {},
                        },
                        Arg );
                    Slots.push_back( std::move( Slot ) );
                }
                return Slots;
            }

            // Bind one declaration that carries `Pending` annotations.
            void BindType ( const TypeDecl &Decl, const std::vector<PendingAnnotation> &Pending )
            {
                // The type exists as an identity first; its layout is an
                // attribute that may stay unresolved (generics, aggregates
                // whose fields are not bound yet). Type checking never needs it.
                const NominalId Id = Store.DeclareType( Decl.Name, Unit, Decl.Id );
                ++Bound;

                Core::SmallVec<Symbol, 2> Params;
                for ( const Symbol Name : *Decl.Generics )
                {
                    Params.PushBack( Store.Intern( Ast.Text( Name ) ) );
                }
                Store.SetParams( Id, std::move( Params ) );
                DeclareMembers( Id, *Decl.Body );

                LayoutId Layout;

                for ( const PendingAnnotation &Anno : Pending )
                {
                    const std::string_view AnnoName = Ast.Text( Anno.Name );

                    if ( AnnoName == "Primitive" and Anno.Args.Size() >= 1 )
                    {
                        const auto Spelling = Frontend::AsStringText( Ast, Anno.Args[0] );
                        if ( not Spelling )
                        {
                            Report( Core::ESeverity::Error, Anno.Loc,
                                    "@[Primitive] expects a layout spelling string, e.g. @[Primitive( \"i32\", 32 )]" );
                            continue;
                        }
                        const std::uint32_t Bits = Anno.Args.Size() >= 2 ? ReadBits( Ast, Anno.Args[1] ) : 0;
                        Layout                   = Store.AddPrimitive( Store.Intern( *Spelling ), Bits );
                    }
                    else if ( AnnoName == "Literal" )
                    {
                        const auto *Kind =
                            Anno.Args.Size() >= 1 ? std::get_if<Frontend::Identifier>( &Ast.Expr( Anno.Args[0] ) ) : nullptr;
                        if ( Kind == nullptr )
                        {
                            Report( Core::ESeverity::Error, Anno.Loc,
                                    "@[Literal] expects a node kind, e.g. @[Literal( IntLiteral )]" );
                            continue;
                        }
                        // Bound here rather than after the loop: a type may
                        // claim several node kinds by repeating the
                        // annotation — `@[Literal( FuncType ), Literal( Lambda )]`
                        // — which is how one stdlib type covers a family of
                        // syntactic shapes that denote the same thing.
                        const std::string_view NodeKind = Ast.Text( Kind->Name );

                        // Two types claiming the same node kind would make the
                        // type of `10` depend on stdlib file order. Refuse.
                        if ( not Store.BindNodeKind( NodeKind, Id ) )
                        {
                            Report( Core::ESeverity::Error, Anno.Loc,
                                    "node kind '" + std::string{ NodeKind } +
                                        "' is already claimed by another type; only one type may wrap it" );
                            continue;
                        }

                        // Extra arguments name the AST fields feeding each
                        // generic parameter; an annotation that carries none
                        // must not wipe what a sibling annotation set.
                        if ( Anno.Args.Size() > 1 )
                        {
                            Store.SetLiteralSlots( Id, ReadLiteralSlots( Anno.Args ) );
                        }
                    }
                }

                // Without @[Primitive] the layout is structural, and only
                // computable for a non-generic whose field types are already
                // bound. Leaving it invalid is correct, not a failure.
                if ( not Layout.IsValid() and not Decl.Body->IsEmpty() )
                {
                    Layout = AggregateOf( *Decl.Body );
                }
                if ( Layout.IsValid() )
                {
                    Store.AttachLayout( Id, Layout );
                }
            }

            // A top-level `def`: same identity-first shape as BindType, minus
            // a layout — a function has no memory representation of its own.
            void BindFunction ( Frontend::DeclId Id, const Frontend::Method &Entry )
            {
                static_cast<void>( Store.DeclareFunction( Ast.Text( Entry.Name ), Unit, Id ) );
                ++Bound;
            }
        };

        // --- Phase B ---------------------------------------------------------

        struct SignatureResolver
        {

            const Frontend::AstContext &Ast;
            TypeStore &Store;
            std::uint32_t Unit   = 0;
            std::size_t Resolved = 0;

            // A written parent link (`< Y<T>`, `include Enumerable<T>`) kept
            // whole. Its arguments live in the *including* type's parameter
            // space, so `include Enumerable<T>` on `Array<T>` stores a
            // reference to parameter 0 — resolved once a receiver says what
            // that T is.
            [[nodiscard]] SigTypeId ParentOf ( Frontend::TypeId Id, std::span<const Symbol> Generics )
            {
                SigSink Sink{ Store };
                return ResolveTypeExpr( Ast, Store, Generics, Sink, Id );
            }

            void Resolve ( const TypeDecl &Decl )
            {
                const auto Found = Store.LookupType( Decl.Name );
                if ( not Found )
                {
                    return;
                }
                const NominalId Id = *Found;

                // Only the unit that declared the type resolves it: a name
                // re-declared elsewhere belongs to whoever won phase A.
                if ( Store.Type( Id ).Unit != Unit or Store.Type( Id ).Decl != Decl.Id )
                {
                    return;
                }

                // The AST's own symbols: ResolveTypeExpr matches a written
                // name against these, and the store's interner is a different
                // table entirely.
                const std::span<const Symbol> Generics{ Decl.Generics->begin(), Decl.Generics->Size() };

                if ( Decl.Super.IsValid() )
                {
                    Store.SetSuper( Id, ParentOf( Decl.Super, Generics ) );
                }

                for ( const Frontend::DeclId Child : *Decl.Body )
                {
                    if ( not Child.IsValid() )
                    {
                        continue;
                    }
                    std::visit(
                        Meta::Overloaded{
                            [&] ( const Frontend::Include &Entry )
                            {
                                if ( const SigTypeId Mixin = ParentOf( Entry.Target, Generics ); Mixin.IsValid() )
                                {
                                    Store.AddInclude( Id, Mixin );
                                }
                            },
                            [&] ( const Frontend::Field &Entry )
                            {
                                SigSink Sink{ Store };
                                const SigTypeId Result = ResolveTypeExpr( Ast, Store, Generics, Sink, Entry.DeclType );
                                if ( Member *Slot = Store.MemberByDecl( Id, Unit, Child ) )
                                {
                                    Slot->Result = Result;
                                    ++Resolved;
                                }
                            },
                            [&] ( const Frontend::Method &Entry )
                            {
                                // One parameter space, the type's followed by
                                // the method's: `def map<U>` on `Array<T>`
                                // resolves T to index 0 and U to index 1, so
                                // a single ParamIndex addresses both and
                                // Instantiate needs no second concept.
                                Core::SmallVec<Symbol, 4> Space;
                                for ( const Symbol Name : *Decl.Generics )
                                {
                                    Space.PushBack( Name );
                                }
                                for ( const Symbol Name : Entry.Generics )
                                {
                                    Space.PushBack( Name );
                                }
                                const std::span<const Symbol> Scope{ Space.begin(), Space.Size() };

                                SigSink Sink{ Store };
                                const SigTypeId Result = ResolveTypeExpr( Ast, Store, Scope, Sink, Entry.ReturnType );
                                Core::SmallVec<SigTypeId, 4> Params;
                                Core::SmallVec<bool, 4> ParamIsBlock;
                                for ( const Frontend::ParamId ParamRef : Entry.Params )
                                {
                                    const Frontend::Param &ParamNode = Ast.GetParam( ParamRef );
                                    Params.PushBack( ResolveTypeExpr( Ast, Store, Scope, Sink, ParamNode.DeclType ) );
                                    ParamIsBlock.PushBack( ParamNode.bIsBlock );
                                }
                                if ( Member *Slot = Store.MemberByDecl( Id, Unit, Child ) )
                                {
                                    Slot->Result       = Result;
                                    Slot->Params       = std::move( Params );
                                    Slot->ParamIsBlock = std::move( ParamIsBlock );
                                    Slot->OwnGenerics  = static_cast<std::uint32_t>( Entry.Generics.Size() );
                                    ++Resolved;
                                }
                            },
                            [&] ( const Frontend::EnumCase &Entry )
                            {
                                // A case is never itself generic — its
                                // payload (`Some( value : T )`) only ever
                                // references the enum's own parameters, so
                                // no space concatenation like Method's.
                                SigSink Sink{ Store };
                                Core::SmallVec<SigTypeId, 4> Params;
                                Core::SmallVec<bool, 4> ParamIsBlock;
                                for ( const Frontend::ParamId ParamRef : Entry.Payload )
                                {
                                    const Frontend::Param &ParamNode = Ast.GetParam( ParamRef );
                                    Params.PushBack( ResolveTypeExpr( Ast, Store, Generics, Sink, ParamNode.DeclType ) );
                                    ParamIsBlock.PushBack( ParamNode.bIsBlock );
                                }
                                if ( Member *Slot = Store.MemberByDecl( Id, Unit, Child ) )
                                {
                                    Slot->Params       = std::move( Params );
                                    Slot->ParamIsBlock = std::move( ParamIsBlock );
                                    ++Resolved;
                                }
                            },
                            [] ( const auto & ) {},
                        },
                        Ast.Decl( Child ) );
                }
            }

            // A top-level `def`: no owner, so its parameter space is its own
            // generics alone — no type generics to prepend.
            void ResolveFunction ( Frontend::DeclId Id, const Frontend::Method &Entry )
            {
                Member *Slot = Store.FunctionByDecl( Unit, Id );
                if ( Slot == nullptr )
                {
                    return;
                }

                const std::span<const Symbol> Scope{ Entry.Generics.begin(), Entry.Generics.Size() };

                SigSink Sink{ Store };
                const SigTypeId Result = ResolveTypeExpr( Ast, Store, Scope, Sink, Entry.ReturnType );
                Core::SmallVec<SigTypeId, 4> Params;
                Core::SmallVec<bool, 4> ParamIsBlock;
                for ( const Frontend::ParamId ParamRef : Entry.Params )
                {
                    const Frontend::Param &ParamNode = Ast.GetParam( ParamRef );
                    Params.PushBack( ResolveTypeExpr( Ast, Store, Scope, Sink, ParamNode.DeclType ) );
                    ParamIsBlock.PushBack( ParamNode.bIsBlock );
                }
                Slot->Result       = Result;
                Slot->Params       = std::move( Params );
                Slot->ParamIsBlock = std::move( ParamIsBlock );
                Slot->OwnGenerics  = static_cast<std::uint32_t>( Entry.Generics.Size() );
                ++Resolved;
            }
        };

    } // namespace

    std::size_t
    BindUnitTypes ( const Frontend::AstContext &Ast, std::uint32_t Unit, TypeStore &Store, Core::DiagEngine::Bag &Diags )
    {
        Binder Bind{ .Ast = Ast, .Store = Store, .Diags = Diags, .Unit = Unit };
        ForEachTypeDecl( Ast, Ast.TopDecls, [&] ( const TypeDecl &Decl, const std::vector<PendingAnnotation> &Pending )
                         { Bind.BindType( Decl, Pending ); } );
        ForEachFreeFunction( Ast, Ast.TopDecls,
                             [&] ( Frontend::DeclId Id, const Frontend::Method &Entry ) { Bind.BindFunction( Id, Entry ); } );
        return Bind.Bound;
    }

    std::size_t
    ResolveUnitSignatures ( const Frontend::AstContext &Ast, std::uint32_t Unit, TypeStore &Store, Core::DiagEngine::Bag &Diags )
    {
        static_cast<void>( Diags );
        SignatureResolver Step{ .Ast = Ast, .Store = Store, .Unit = Unit };
        ForEachTypeDecl( Ast, Ast.TopDecls,
                         [&] ( const TypeDecl &Decl, const std::vector<PendingAnnotation> & ) { Step.Resolve( Decl ); } );
        ForEachFreeFunction( Ast, Ast.TopDecls,
                             [&] ( Frontend::DeclId Id, const Frontend::Method &Entry ) { Step.ResolveFunction( Id, Entry ); } );
        return Step.Resolved;
    }

} // namespace Sema

} // namespace Volt
