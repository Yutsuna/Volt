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
// It runs in three phases, all serial: declaring every name (phase A) must
// complete across all units before any signature is resolved (phase B), or a
// member's return type would depend on stdlib file order. A third phase,
// `ResolveStructLayouts`, attaches every non-`@[Primitive]` type's structural
// `Layout` after phase A — a field may name an aggregate declared in another
// file with no fixed order relative to this one, so it resolves lazily and
// recursively across units rather than in either per-unit pass.

#include "Volt/Sema/Layout/TypeBinder.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Type.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

#include <charconv>
#include <optional>
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
        //
        // Annotations accumulate onto the `def` they precede exactly as they
        // do in ForEachTypeDecl: `@[External( "libc", "malloc" )]` sits on a
        // free function in the stdlib, so dropping them here would lose every
        // C symbol in the build.
        template <typename DeclContainer, typename Fn>
        void ForEachFreeFunction ( const Frontend::AstContext &Ast, const DeclContainer &Decls, Fn &&Visit )
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

                std::visit(
                    Meta::Overloaded{
                        [&] ( const Frontend::Module &Nested ) { ForEachFreeFunction( Ast, Nested.Body, Visit ); },
                        [&] ( const Frontend::Method &Entry ) { Visit( Id, Entry, Pending ); },
                        [] ( const auto & ) {},
                    },
                    Node );

                Pending.clear();
            }
        }

        // Record every module name, nested ones included. Names only — a
        // module has no members of its own: ForEachTypeDecl already hoisted
        // its types and ForEachFreeFunction its methods. See
        // TypeStore::DeclareModule for why the name alone is worth keeping.
        template <typename DeclContainer>
        void DeclareModules ( const Frontend::AstContext &Ast, const DeclContainer &Decls, TypeStore &Store )
        {
            for ( const Frontend::DeclId Id : Decls )
            {
                if ( not Id.IsValid() )
                {
                    continue;
                }
                if ( const auto *Nested = std::get_if<Frontend::Module>( &Ast.Decl( Id ) ) )
                {
                    Store.DeclareModule( Ast.Text( Nested->Name ) );
                    DeclareModules( Ast, Nested->Body, Store );
                }
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

        // `@[External( "libc" [, "malloc"] )]` — the library to link and the C
        // symbol to link against. The symbol defaults to the declaration's own
        // spelling, which is the common case (`external def memcpy`); Volt
        // only names it explicitly when the two differ (`libc_malloc` wrapping
        // `malloc`, because a bare `malloc` inside a struct would name that
        // struct's own method).
        //
        // Nothing here is a Volt type name: the strings are link-time
        // identifiers, exactly like `@[Primitive]`'s opaque spelling
        // (rules/zero-hardcode.md).
        void ReadExternal ( const Frontend::AstContext &Ast,
                            TypeStore &Store,
                            const std::vector<PendingAnnotation> &Pending,
                            std::string_view DefaultSymbol,
                            Member &Slot )
        {
            for ( const PendingAnnotation &Anno : Pending )
            {
                if ( Ast.Text( Anno.Name ) != "External" )
                {
                    continue;
                }

                const auto Library = Anno.Args.Size() >= 1 ? Frontend::AsStringText( Ast, Anno.Args[0] ) : std::nullopt;
                const auto Symbol  = Anno.Args.Size() >= 2 ? Frontend::AsStringText( Ast, Anno.Args[1] ) : std::nullopt;

                Slot.ExternLib    = Store.Intern( Library.value_or( std::string_view{} ) );
                Slot.ExternSymbol = Store.Intern( Symbol.value_or( DefaultSymbol ) );
            }
        }

        // The type a field's declared type names, if any — dropping any
        // generic arguments written (`Pointer<UInt8>` names `Pointer`), the
        // same restriction `AggregateOf`'s caller already lives with: a type
        // whose *own* layout depends on a generic argument stays invalid
        // regardless (materialising that is codegen's job, BackendCore's
        // InstanceLayouts — rules/core-ast.md).
        [[nodiscard]] std::optional<NominalId>
        FieldTypeNominal ( const Frontend::AstContext &Ast, const TypeStore &Store, Frontend::TypeId Id )
        {
            if ( not Id.IsValid() )
            {
                return std::nullopt;
            }
            const auto *Ref = std::get_if<Frontend::TypeRef>( &Ast.Type( Id ) );
            if ( Ref == nullptr or Ref->Path.Size() == 0 )
            {
                return std::nullopt;
            }
            return Store.LookupType( Ast.Text( Ref->Path[Ref->Path.Size() - 1] ) );
        }

        // `Struct`/`Class`/`Mixin`/`Enum` collapse to one `NominalType` shape
        // at bind time (this file's own `TypeDecl`), so recovering "what is
        // this type's body" from a bare `NominalId` — which is all a field
        // reference carries — has to re-open that variant.
        [[nodiscard]] const Frontend::DeclList *TypeBodyOf ( const Frontend::AstContext &Ast, Frontend::DeclId Decl )
        {
            return std::visit(
                Meta::Overloaded{
                    [] ( const Frontend::Struct &Type ) -> const Frontend::DeclList * { return &Type.Body; },
                    [] ( const Frontend::Class &Type ) -> const Frontend::DeclList * { return &Type.Body; },
                    [] ( const Frontend::Mixin &Type ) -> const Frontend::DeclList * { return &Type.Body; },
                    [] ( const Frontend::Enum &Type ) -> const Frontend::DeclList * { return &Type.Body; },
                    [] ( const auto & ) -> const Frontend::DeclList * { return nullptr; },
                },
                Ast.Decl( Decl ) );
        }

        // The written `< Parent`, read off the AST rather than
        // `NominalType::Super` — SignatureResolver only fills that in Phase B,
        // and layouts are attached before any signature is resolved
        // (Driver.cpp). `class` is the only declaration that can name one.
        [[nodiscard]] Frontend::TypeId TypeSuperOf ( const Frontend::AstContext &Ast, Frontend::DeclId Decl )
        {
            const auto *Type = std::get_if<Frontend::Class>( &Ast.Decl( Decl ) );
            return Type != nullptr ? Type->Super : Frontend::TypeId{};
        }

        // A cross-file `Aggregate` may recurse arbitrarily (`Exception`'s
        // `message : String`, and `String` could in principle name a further
        // aggregate); a genuine by-value cycle is not a layout Volt can build
        // at all (infinite size), so depth is bounded the same way every
        // other bounded walk in this codebase is (`TypeStore::LookupMember`,
        // `InstanceLayouts`), and a cycle simply resolves to an invalid field
        // rather than recursing forever.
        constexpr std::uint32_t MaxLayoutDepth = 32;

        // The aggregate a struct/class/mixin/enum collapses to when it has no
        // `@[Primitive]` — memoised onto the store itself (`AttachLayout`),
        // so a type reached from two different fields is only ever built
        // once, and so this doubles as the "already resolved" check that
        // lets two aggregates name each other's fields in whichever order the
        // outer `ResolveStructLayouts` loop reaches them. `Units` is indexed
        // by the declaring unit's ordinal, so a field naming a type from a
        // different file — the case no single per-unit pass can answer while
        // reading only its own AST — recurses across into that file's own
        // AST directly.
        LayoutId EnsureStructLayout ( std::span<const Frontend::AstContext *const> Units,
                                      TypeStore &Store,
                                      NominalId Id,
                                      std::uint32_t Depth )
        {
            if ( not Id.IsValid() or Depth > MaxLayoutDepth )
            {
                return LayoutId{};
            }

            const NominalType &Type = Store.Type( Id );
            if ( Type.Layout.IsValid() )
            {
                // `@[Primitive]` (Phase A already attached it) or an
                // aggregate a sibling recursion already built.
                return Type.Layout;
            }
            if ( Type.Unit >= Units.size() or Units[Type.Unit] == nullptr )
            {
                return LayoutId{};
            }

            const Frontend::AstContext &Ast = *Units[Type.Unit];
            const Frontend::DeclList *Body  = TypeBodyOf( Ast, Type.Decl );
            if ( Body == nullptr )
            {
                return LayoutId{};
            }

            Aggregate Agg;

            // A subclass's storage *is* one of its base: `super( message )`
            // hands `self` straight to `Exception#initialize`, which GEPs
            // `@message` at the offset *Exception's* layout gives it. So the
            // base's fields lead, and they are spliced flat rather than nested
            // — an inherited `@x` is looked up by name in the subclass's own
            // layout (FieldAddress), and a nested base would hide every one of
            // them. Without this a subclass laid out as `{}`: every
            // `raise ArgumentError.new( "..." )` had `Exception#initialize`
            // write 40 bytes into a zero-byte frame slot.
            if ( const std::optional<NominalId> Parent = FieldTypeNominal( Ast, Store, TypeSuperOf( Ast, Type.Decl ) ) )
            {
                const LayoutId Inherited = EnsureStructLayout( Units, Store, *Parent, Depth + 1 );
                if ( const auto *Base = Inherited.IsValid() ? std::get_if<Aggregate>( &Store.Get( Inherited ) ) : nullptr;
                     Base != nullptr )
                {
                    for ( const FieldLayout &Field : Base->Fields )
                    {
                        Agg.Fields.PushBack( Field );
                    }
                }
            }

            bool bFullyResolved = true;
            for ( const Frontend::DeclId Child : *Body )
            {
                const auto *Field = std::get_if<Frontend::Field>( &Ast.Decl( Child ) );
                if ( Field == nullptr )
                {
                    continue;
                }
                LayoutId FieldType;
                if ( const std::optional<NominalId> Named = FieldTypeNominal( Ast, Store, Field->DeclType ) )
                {
                    FieldType = EnsureStructLayout( Units, Store, *Named, Depth + 1 );
                }
                if ( not FieldType.IsValid() )
                {
                    bFullyResolved = false;
                }
                Agg.Fields.PushBack( FieldLayout{ .Name = Store.Intern( Ast.Text( Field->Name ) ), .Type = FieldType } );
            }

            // A field typed as a bare generic parameter (`key : K` on
            // `HashEntry<K, V>`, `first : T` on `Range<T>`) resolves no
            // FieldTypeNominal at all — by design, per NominalType::Layout's
            // own contract above: "never [attached] for a generic whose shape
            // depends on its arguments". Attaching this broken template
            // anyway would make InstanceLayouts::Of's `Attached.IsValid()`
            // fast path (BackendCore/InstanceLayout.cpp) return it verbatim,
            // permanently short-circuiting the substitution that turns
            // ParamIndex-encoded field signatures into `T`'s concrete
            // argument. Leaving Layout unattached instead defers every such
            // type to that substitution, exactly as the comment promises.
            if ( not bFullyResolved )
            {
                return LayoutId{};
            }

            const LayoutId Built = Store.AddAggregate( std::move( Agg ) );
            Store.AttachLayout( Id, Built );
            return Built;
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
                std::vector<PendingAnnotation> Pending;

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
                        Pending.push_back( PendingAnnotation{ .Name = Anno->Name, .Args = Anno->Args, .Loc = Anno->Loc } );
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
                                Slot.bAbstract = Entry.bAbstract;
                                ReadExternal( Ast, Store, Pending, Ast.Text( Entry.Name ), Slot );
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

                    Pending.clear();
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

                // Without @[Primitive] the layout is structural — a field may
                // name a type any other file declares, so it is computed in
                // Phase B (SignatureResolver::Resolve) once every unit's
                // Phase A has run and every name this build declares exists,
                // rather than here. Leaving it invalid until then is correct,
                // not a failure.
                if ( Layout.IsValid() )
                {
                    Store.AttachLayout( Id, Layout );
                }
            }

            // A top-level `def`: same identity-first shape as BindType, minus
            // a layout — a function has no memory representation of its own.
            void
            BindFunction ( Frontend::DeclId Id, const Frontend::Method &Entry, const std::vector<PendingAnnotation> &Pending )
            {
                Member *Slot = Store.DeclareFunction( Ast.Text( Entry.Name ), Unit, Id );
                ReadExternal( Ast, Store, Pending, Ast.Text( Entry.Name ), *Slot );
                ++Bound;
            }
        };

        // --- Phase B ---------------------------------------------------------

        struct SignatureResolver
        {

            const Frontend::AstContext &Ast;
            TypeStore &Store;
            Core::DiagEngine::Bag &Diags;
            std::uint32_t Unit   = 0;
            std::size_t Resolved = 0;

            void Report ( Core::ESeverity Severity, Core::SourceRange Loc, const std::string &Message )
            {
                Diags.Report( Core::Diagnostic{ .Severity = Severity, .Range = Loc, .Message = Message, .Notes = {} } );
            }

            // A defaulted parameter followed by a non-defaulted one would
            // make positional argument matching silently wrong — the caller
            // can never supply the later required slot without also
            // supplying the earlier optional one. `&block` never occupies a
            // positional slot, so it neither counts as defaulted nor breaks
            // the run of defaults before it.
            void CheckTrailingDefaults ( const Frontend::ParamList &Params )
            {
                bool bSeenDefault = false;
                for ( const Frontend::ParamId ParamRef : Params )
                {
                    const Frontend::Param &ParamNode = Ast.GetParam( ParamRef );
                    if ( ParamNode.bIsBlock )
                    {
                        continue;
                    }
                    if ( ParamNode.Default.IsValid() )
                    {
                        bSeenDefault = true;
                        continue;
                    }
                    if ( bSeenDefault )
                    {
                        Report( Core::ESeverity::Error, ParamNode.Loc,
                                "parameter '" + std::string{ Ast.Text( ParamNode.Name ) } +
                                    "' has no default value but follows a parameter that does" );
                    }
                }
            }

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
                const auto Found = Store.LookupTypeByDecl( Unit, Decl.Id );
                if ( not Found )
                {
                    return;
                }
                const NominalId Id = *Found;

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

                                CheckTrailingDefaults( Entry.Params );

                                SigSink Sink{ Store };
                                const SigTypeId Result = ResolveTypeExpr( Ast, Store, Scope, Sink, Entry.ReturnType );
                                Core::SmallVec<SigTypeId, 4> Params;
                                Core::SmallVec<bool, 4> ParamIsBlock;
                                std::uint32_t MinParams = 0;
                                for ( const Frontend::ParamId ParamRef : Entry.Params )
                                {
                                    const Frontend::Param &ParamNode = Ast.GetParam( ParamRef );
                                    Params.PushBack( ResolveTypeExpr( Ast, Store, Scope, Sink, ParamNode.DeclType ) );
                                    ParamIsBlock.PushBack( ParamNode.bIsBlock );
                                    if ( not ParamNode.bIsBlock and not ParamNode.Default.IsValid() )
                                    {
                                        ++MinParams;
                                    }
                                }
                                if ( Member *Slot = Store.MemberByDecl( Id, Unit, Child ) )
                                {
                                    Slot->Result       = Result;
                                    Slot->Params       = std::move( Params );
                                    Slot->ParamIsBlock = std::move( ParamIsBlock );
                                    Slot->OwnGenerics  = static_cast<std::uint32_t>( Entry.Generics.Size() );
                                    Slot->MinParams    = MinParams;
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

                CheckTrailingDefaults( Entry.Params );

                const std::span<const Symbol> Scope{ Entry.Generics.begin(), Entry.Generics.Size() };

                SigSink Sink{ Store };
                const SigTypeId Result = ResolveTypeExpr( Ast, Store, Scope, Sink, Entry.ReturnType );
                Core::SmallVec<SigTypeId, 4> Params;
                Core::SmallVec<bool, 4> ParamIsBlock;
                std::uint32_t MinParams = 0;
                for ( const Frontend::ParamId ParamRef : Entry.Params )
                {
                    const Frontend::Param &ParamNode = Ast.GetParam( ParamRef );
                    Params.PushBack( ResolveTypeExpr( Ast, Store, Scope, Sink, ParamNode.DeclType ) );
                    ParamIsBlock.PushBack( ParamNode.bIsBlock );
                    if ( not ParamNode.bIsBlock and not ParamNode.Default.IsValid() )
                    {
                        ++MinParams;
                    }
                }
                Slot->Result       = Result;
                Slot->Params       = std::move( Params );
                Slot->ParamIsBlock = std::move( ParamIsBlock );
                Slot->OwnGenerics  = static_cast<std::uint32_t>( Entry.Generics.Size() );
                Slot->MinParams    = MinParams;
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
                             [&] ( Frontend::DeclId Id, const Frontend::Method &Entry,
                                   const std::vector<PendingAnnotation> &Pending ) { Bind.BindFunction( Id, Entry, Pending ); } );
        DeclareModules( Ast, Ast.TopDecls, Store );
        return Bind.Bound;
    }

    std::size_t
    ResolveUnitSignatures ( const Frontend::AstContext &Ast, std::uint32_t Unit, TypeStore &Store, Core::DiagEngine::Bag &Diags )
    {
        SignatureResolver Step{ .Ast = Ast, .Store = Store, .Diags = Diags, .Unit = Unit };
        ForEachTypeDecl( Ast, Ast.TopDecls,
                         [&] ( const TypeDecl &Decl, const std::vector<PendingAnnotation> & ) { Step.Resolve( Decl ); } );
        ForEachFreeFunction( Ast, Ast.TopDecls,
                             [&] ( Frontend::DeclId Id, const Frontend::Method &Entry, const std::vector<PendingAnnotation> & )
                             { Step.ResolveFunction( Id, Entry ); } );
        return Step.Resolved;
    }

    void ResolveStructLayouts ( std::span<const Frontend::AstContext *const> Units, TypeStore &Store )
    {
        for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
        {
            const NominalId Id{ static_cast<NominalId::ValueType>( Index ) };
            static_cast<void>( EnsureStructLayout( Units, Store, Id, 0 ) );
        }
    }

} // namespace Sema

} // namespace Volt
