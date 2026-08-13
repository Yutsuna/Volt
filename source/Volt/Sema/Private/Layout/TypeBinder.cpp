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

#include "Raii/Ownership.hpp"
#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Expr.hpp"
#include "Volt/Frontend/AST/Type.hpp"
#include "Volt/Sema/Layout/TypeResolve.hpp"

#include <algorithm>
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
            // Parallel to Generics; null for a shape that does not carry
            // bounds yet (Enum — see AST Decl.hpp). A non-null list may still
            // be empty (no generics at all) or hold invalid TypeIds
            // (unbounded parameters).
            const Frontend::TypeList *GenericBounds = nullptr;
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
                            Visit( TypeDecl{ .Name          = Ast.Text( Type.Name ),
                                             .Id            = Id,
                                             .Generics      = &Type.Generics,
                                             .Body          = &Type.Body,
                                             .Super         = Frontend::TypeId{},
                                             .GenericBounds = &Type.GenericBounds },
                                   Pending );
                        },
                        [&] ( const Frontend::Class &Type )
                        {
                            Visit( TypeDecl{ .Name          = Ast.Text( Type.Name ),
                                             .Id            = Id,
                                             .Generics      = &Type.Generics,
                                             .Body          = &Type.Body,
                                             .Super         = Type.Super,
                                             .GenericBounds = &Type.GenericBounds },
                                   Pending );
                        },
                        [&] ( const Frontend::Mixin &Type )
                        {
                            Visit( TypeDecl{ .Name          = Ast.Text( Type.Name ),
                                             .Id            = Id,
                                             .Generics      = &Type.Generics,
                                             .Body          = &Type.Body,
                                             .Super         = Frontend::TypeId{},
                                             .GenericBounds = &Type.GenericBounds },
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

        // An `EnumCase::Value` (a materialized `IntLiteral`, always valid by
        // now — `Frontend::MaterializeEnumOrdinals` runs before this file's
        // Phase A ever does, see `EnumSynthesis.hpp`). Malformed or missing
        // decodes to 0, the same permissive fallback `ReadBits` above uses.
        [[nodiscard]] std::int64_t DecodeEnumOrdinal ( const Frontend::AstContext &Ast, Frontend::ExprId Id )
        {
            const auto *Literal = Id.IsValid() ? std::get_if<Frontend::IntLiteral>( &Ast.Expr( Id ) ) : nullptr;
            if ( Literal == nullptr )
            {
                return 0;
            }

            const std::string_view Raw = Ast.Text( Literal->Raw );
            std::int64_t Value         = 0;
            std::from_chars( Raw.data(), Raw.data() + Raw.size(), Value );
            return Value;
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

        // Resolves the nominal an enum's tag/underlying value collapses to:
        // the type its own `: Underlying` names, or — unwritten (`Color`,
        // `TaskStatus`) — whichever nominal claims `IntLiteral`, the same
        // default that gives a bare `10` its type. Zero-hardcode: no Volt
        // type name is spelled here, only a node-kind claim already used
        // for that exact purpose elsewhere (`TypeCompat`'s `nil`, `Pointer`'s
        // pointee).
        [[nodiscard]] std::optional<NominalId>
        EnumTagNominal ( const Frontend::AstContext &Ast, const TypeStore &Store, const Frontend::Enum &Type )
        {
            if ( Type.Underlying.IsValid() )
            {
                return FieldTypeNominal( Ast, Store, Type.Underlying );
            }
            return Store.LookupNodeKind( "IntLiteral" );
        }

        // An enum's layout: `Primitive` (the tag alone) when no case carries
        // a payload — `self` *is* the ordinal/explicit value, exactly like
        // `Symbol`'s `@[Primitive("u64",64)]` — or `Aggregate{ tag,
        // ...payload fields }` when at least one case does (`Optional<T>`'s
        // `Some(value: T)`). Payload fields are flattened across every case
        // rather than sharing storage: the one payload-bearing case in the
        // corpus has a single variant, and a real union would need a new
        // `LayoutKind` reaching every `Aggregate` consumer for no benefit
        // yet — revisit only if a multi-payload ADT needs it.
        //
        // Declared forward of `EnsureStructLayout` isn't possible (it calls
        // back into it for the tag/payload nominals), so this is defined
        // just after it instead; see the call site inside `EnsureStructLayout`.
        LayoutId EnsureEnumLayout ( std::span<const Frontend::AstContext *const> Units,
                                    TypeStore &Store,
                                    NominalId Id,
                                    const Frontend::AstContext &Ast,
                                    const Frontend::Enum &Type,
                                    std::uint32_t Depth,
                                    std::vector<NominalId> &ActiveStack,
                                    Core::DiagEngine::Bag *Diags );

        // A non-generic `SigType` wrapping a plain nominal — no ParamIndex,
        // no Base's own generic arguments threaded through. Used for
        // `to_value`'s synthesized Result (see `EnumSynthesis.hpp`), where
        // the answer is simply "whatever nominal the enum's tag resolves
        // to," never something depending on the enum's own generics.
        [[nodiscard]] SigTypeId PlainSigOf ( TypeStore &Store, NominalId Id )
        {
            SigType Sig;
            Sig.Base = Id;
            return Store.AddSig( std::move( Sig ) );
        }

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
                                      std::uint32_t Depth,
                                      std::vector<NominalId> &ActiveStack,
                                      Core::DiagEngine::Bag *Diags )
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

            if ( std::find( ActiveStack.begin(), ActiveStack.end(), Id ) != ActiveStack.end() )
            {
                if ( Diags != nullptr and Type.Unit < Units.size() and Units[Type.Unit] != nullptr )
                {
                    const Frontend::AstContext &Ast = *Units[Type.Unit];
                    const std::string_view TypeName = Store.Text( Type.Name );
                    Diags->Report( Core::Diagnostic{ .Severity = Core::ESeverity::Error,
                                                     .Range    = Frontend::LocOf( Ast.Decl( Type.Decl ) ).Head(),
                                                     .Message  = "recursive type '" + std::string( TypeName ) +
                                                                "' has infinite size (layout cycle detected)",
                                                     .Notes = {} } );
                }
                return LayoutId{};
            }

            ActiveStack.push_back( Id );
            struct StackGuard
            {
                std::vector<NominalId> &Stack;
                ~StackGuard ()
                {
                    Stack.pop_back();
                }
            } Guard{ ActiveStack };

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

            // An `Enum` never has a `Field`/`Super` — its layout is either a
            // bare tag or a tag-plus-payload aggregate, never the
            // inheritance-splicing shape below.
            if ( const auto *EnumType = std::get_if<Frontend::Enum>( &Ast.Decl( Type.Decl ) ) )
            {
                return EnsureEnumLayout( Units, Store, Id, Ast, *EnumType, Depth, ActiveStack, Diags );
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
                const LayoutId Inherited = EnsureStructLayout( Units, Store, *Parent, Depth + 1, ActiveStack, Diags );
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
                    FieldType = EnsureStructLayout( Units, Store, *Named, Depth + 1, ActiveStack, Diags );
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

        LayoutId EnsureEnumLayout ( std::span<const Frontend::AstContext *const> Units,
                                    TypeStore &Store,
                                    NominalId Id,
                                    const Frontend::AstContext &Ast,
                                    const Frontend::Enum &Type,
                                    std::uint32_t Depth,
                                    std::vector<NominalId> &ActiveStack,
                                    Core::DiagEngine::Bag *Diags )
        {
            const std::optional<NominalId> TagNominal = EnumTagNominal( Ast, Store, Type );
            const LayoutId TagLayout =
                TagNominal ? EnsureStructLayout( Units, Store, *TagNominal, Depth + 1, ActiveStack, Diags ) : LayoutId{};

            if ( not TagLayout.IsValid() )
            {
                // The underlying/default nominal hasn't resolved its own
                // `@[Primitive]` yet (cross-unit ordering) — leave this
                // enum's Layout invalid too, exactly like a struct field
                // naming an unresolved aggregate; `ResolveStructLayouts`'s
                // outer loop or a later recursive reach will retry it.
                return LayoutId{};
            }

            bool bHasPayload = false;
            for ( const Frontend::DeclId Child : Type.Body )
            {
                const auto *Case = Child.IsValid() ? std::get_if<Frontend::EnumCase>( &Ast.Decl( Child ) ) : nullptr;
                if ( Case != nullptr and Case->Payload.Size() > 0 )
                {
                    bHasPayload = true;
                    break;
                }
            }

            if ( not bHasPayload )
            {
                Store.AttachLayout( Id, TagLayout );
                return TagLayout;
            }

            Aggregate Agg;
            Agg.Fields.PushBack( FieldLayout{ .Name = Store.Intern( "tag" ), .Type = TagLayout } );

            // A payload field is named from its *case*, not its written
            // parameter name (`value` in `Some( value : T )`) — deliberately,
            // so a generic enum's per-instantiation layout
            // (`InstanceLayout::Of`, which only ever sees a `TypeStore`, no
            // AST) can derive the identical field name from `Member::Name`
            // alone. One field → the case name itself (`Some`); more than one
            // → `<CaseName>_<Index>`. Keep both sites of this rule in sync.
            bool bFullyResolved = true;
            for ( const Frontend::DeclId Child : Type.Body )
            {
                const auto *Case = Child.IsValid() ? std::get_if<Frontend::EnumCase>( &Ast.Decl( Child ) ) : nullptr;
                if ( Case == nullptr )
                {
                    continue;
                }
                const std::string_view CaseName = Ast.Text( Case->Name );
                std::uint32_t ParamIndex        = 0;
                for ( const Frontend::ParamId ParamRef : Case->Payload )
                {
                    const Frontend::Param &ParamNode = Ast.GetParam( ParamRef );
                    LayoutId FieldType;
                    if ( const std::optional<NominalId> Named = FieldTypeNominal( Ast, Store, ParamNode.DeclType ) )
                    {
                        FieldType = EnsureStructLayout( Units, Store, *Named, Depth + 1, ActiveStack, Diags );
                    }
                    if ( not FieldType.IsValid() )
                    {
                        bFullyResolved = false;
                    }
                    // `$`-prefixed: an EnumCase member is *also* named
                    // `CaseName` (`Some`) — `LookupMemberOn`/`OwnMember`
                    // return the first name match, so a payload field
                    // sharing the bare case name would silently resolve to
                    // the wrong member (the case's own self-constructing
                    // signature, not the field) the moment both exist in
                    // the same Members vector. `$` cannot open a written
                    // Volt identifier, so it can never collide with a real
                    // field/case/method name.
                    const std::string FieldName = Case->Payload.Size() > 1
                                                      ? "$" + std::string{ CaseName } + "_" + std::to_string( ParamIndex )
                                                      : "$" + std::string{ CaseName };
                    Agg.Fields.PushBack( FieldLayout{ .Name = Store.Intern( FieldName ), .Type = FieldType } );
                    ++ParamIndex;
                }
            }

            // A generic enum's payload naming a bare parameter (`Some(value:
            // T)` on `Optional<T>`) never resolves here — same contract as a
            // generic struct field (`EnsureStructLayout`'s own comment
            // above): deferred to `InstanceLayouts` at instantiation time.
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
                                Slot.Name        = Store.Intern( Ast.Text( Entry.Name ) );
                                Slot.Kind        = EMemberKind::EnumCase;
                                Slot.Unit        = Unit;
                                Slot.Decl        = Child;
                                Slot.Result      = SelfSigOf( Id );
                                Slot.EnumOrdinal = DecodeEnumOrdinal( Ast, Entry.Value );
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

                // `T : Bound` — resolved here, not Phase A, for the same
                // reason as Super/Includes: the bound's own name may live in
                // a different unit, so it is only guaranteed to exist once
                // every unit's Phase A has run. Kept in this type's own
                // parameter space (ParentOf / SigSink), exactly like a
                // written parent link.
                if ( Decl.GenericBounds != nullptr )
                {
                    Core::SmallVec<SigTypeId, 2> Bounds;
                    for ( const Frontend::TypeId Bound : *Decl.GenericBounds )
                    {
                        Bounds.PushBack( Bound.IsValid() ? ParentOf( Bound, Generics ) : SigTypeId{} );
                    }
                    Store.SetParamBounds( Id, std::move( Bounds ) );
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

                                // `to_value` (`EnumSynthesis.cpp`) is
                                // synthesized with no written return type —
                                // there is no Volt type name to spell at
                                // parse time for an enum with no `:
                                // Underlying`. Resolve it here instead, the
                                // same way the enum's own tag layout is
                                // resolved (`EnumTagNominal`): by now every
                                // unit's Phase A has run, so
                                // `LookupNodeKind("IntLiteral")` is
                                // guaranteed populated.
                                SigTypeId Result;
                                const auto *EnumType = not Entry.ReturnType.IsValid()
                                                           ? std::get_if<Frontend::Enum>( &Ast.Decl( Decl.Id ) )
                                                           : nullptr;
                                if ( EnumType != nullptr and Ast.Text( Entry.Name ) == "to_value" )
                                {
                                    if ( const std::optional<NominalId> TagNominal = EnumTagNominal( Ast, Store, *EnumType ) )
                                    {
                                        Result = PlainSigOf( Store, *TagNominal );
                                    }
                                }
                                else
                                {
                                    Result = ResolveTypeExpr( Ast, Store, Scope, Sink, Entry.ReturnType );
                                }

                                Core::SmallVec<SigTypeId, 4> Params;
                                Core::SmallVec<bool, 4> ParamIsBlock;
                                std::uint32_t MinParams = 0;
                                for ( const Frontend::ParamId ParamRef : Entry.Params )
                                {
                                    Frontend::Param &ParamNode = const_cast<Frontend::AstContext &>( Ast ).GetParam( ParamRef );
                                    SigTypeId ParamSig;

                                    if ( ParamNode.bInstanceVar )
                                    {
                                        if ( not ParamNode.DeclType.IsValid() )
                                        {
                                            Frontend::TypeId InferredDeclType;
                                            if ( Decl.Body != nullptr )
                                            {
                                                for ( const Frontend::DeclId ChildId : *Decl.Body )
                                                {
                                                    if ( not ChildId.IsValid() )
                                                    {
                                                        continue;
                                                    }
                                                    if ( const auto *FieldNode =
                                                             std::get_if<Frontend::Field>( &Ast.Decl( ChildId ) ) )
                                                    {
                                                        if ( FieldNode->Name == ParamNode.Name and FieldNode->DeclType.IsValid() )
                                                        {
                                                            InferredDeclType = FieldNode->DeclType;
                                                            break;
                                                        }
                                                    }
                                                }
                                            }

                                            if ( InferredDeclType.IsValid() )
                                            {
                                                ParamNode.DeclType = InferredDeclType;
                                            }
                                            else
                                            {
                                                const auto FoundField = Store.LookupMember( Id, Ast.Text( ParamNode.Name ) );
                                                if ( FoundField.Decl != nullptr and FoundField.Decl->Kind == EMemberKind::Field )
                                                {
                                                    if ( FoundField.Decl->Result.IsValid() )
                                                    {
                                                        ParamSig = FoundField.Decl->Result;
                                                    }
                                                    if ( FoundField.Decl->Decl.IsValid() )
                                                    {
                                                        if ( const auto *FieldNode = std::get_if<Frontend::Field>(
                                                                 &Ast.Decl( FoundField.Decl->Decl ) ) )
                                                        {
                                                            if ( FieldNode->DeclType.IsValid() )
                                                            {
                                                                ParamNode.DeclType = FieldNode->DeclType;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        if ( not ParamSig.IsValid() and ParamNode.DeclType.IsValid() )
                                        {
                                            ParamSig = ResolveTypeExpr( Ast, Store, Scope, Sink, ParamNode.DeclType );
                                        }

                                        if ( not ParamSig.IsValid() )
                                        {
                                            Report( Core::ESeverity::Error, ParamNode.Loc,
                                                    "parameter '@" + std::string{ Ast.Text( ParamNode.Name ) } +
                                                        "' has no corresponding field declared on '" + std::string{ Decl.Name } + "'" );
                                        }
                                    }
                                    else if ( not ParamNode.bIsBlock and not ParamNode.DeclType.IsValid() )
                                    {
                                        Report( Core::ESeverity::Error, ParamNode.Loc,
                                                "parameter '" + std::string{ Ast.Text( ParamNode.Name ) } +
                                                    "' requires an explicit type annotation" );
                                    }
                                    else
                                    {
                                        ParamSig = ResolveTypeExpr( Ast, Store, Scope, Sink, ParamNode.DeclType );
                                        if ( not ParamNode.bIsBlock and not ParamSig.IsValid() )
                                        {
                                            Report( Core::ESeverity::Error, ParamNode.Loc,
                                                    "parameter '" + std::string{ Ast.Text( ParamNode.Name ) } +
                                                        "' has unresolvable type" );
                                        }
                                    }

                                    Params.PushBack( ParamSig );
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

                // A payload-bearing enum's `tag` and per-case payload slots
                // (`Optional<T>`'s `Some`) are real `Field`-kind Members,
                // not just `Aggregate` layout entries (`EnsureEnumLayout`,
                // `InstanceLayout::Of`) — a *generic* enum never gets its
                // own attached Layout at all, so registering these only
                // there would leave `tmp.tag`/`self.Some` unresolvable
                // through the ordinary `MemberType`/`LookupOn` path, which
                // is exactly the path `Sema::ReinstantiateBody` re-walks
                // per instantiation over a *fresh* `UnitTypes` overlay — a
                // node whose type was merely stamped once, by hand, on the
                // file's own overlay (`Sema/Private/Passes/TypeChecker/
                // EnumCaseLowering.cpp`) is untyped again there. Registered
                // here, once per enum, they resolve exactly like any other
                // struct field, under both the original walk and every
                // re-instantiation.
                if ( const auto *EnumType = std::get_if<Frontend::Enum>( &Ast.Decl( Decl.Id ) ) )
                {
                    bool bHasPayload = false;
                    for ( const Member &Existing : Store.Type( Id ).Members )
                    {
                        if ( Existing.Kind == EMemberKind::EnumCase and Existing.Params.Size() > 0 )
                        {
                            bHasPayload = true;
                            break;
                        }
                    }

                    if ( bHasPayload )
                    {
                        if ( const std::optional<NominalId> TagNominal = EnumTagNominal( Ast, Store, *EnumType ) )
                        {
                            Member TagField;
                            TagField.Kind   = EMemberKind::Field;
                            TagField.Name   = Store.Intern( "tag" );
                            TagField.Result = PlainSigOf( Store, *TagNominal );
                            Store.AddMember( Id, std::move( TagField ) );
                        }

                        // Snapshotted before any further AddMember() below —
                        // those grow the very vector Store.Type(Id).Members
                        // is a live reference into.
                        std::vector<std::pair<std::string, SigTypeId>> PayloadFields;
                        for ( const Member &Existing : Store.Type( Id ).Members )
                        {
                            if ( Existing.Kind != EMemberKind::EnumCase )
                            {
                                continue;
                            }
                            for ( std::size_t Index = 0; Index < Existing.Params.Size(); ++Index )
                            {
                                // `$`-prefixed — see the identical naming
                                // note in `EnsureEnumLayout` above: without
                                // it this Field member's name collides with
                                // the EnumCase member of the same name
                                // already in this same Members vector, and
                                // `OwnMember`'s first-match lookup silently
                                // resolves `self.Some` to the wrong one.
                                const std::string FieldName =
                                    Existing.Params.Size() > 1
                                        ? "$" + std::string{ Store.Text( Existing.Name ) } + "_" + std::to_string( Index )
                                        : "$" + std::string{ Store.Text( Existing.Name ) };
                                PayloadFields.emplace_back( FieldName, Existing.Params[Index] );
                            }
                        }
                        for ( auto &[FieldName, ResultSig] : PayloadFields )
                        {
                            Member PayloadField;
                            PayloadField.Kind   = EMemberKind::Field;
                            PayloadField.Name   = Store.Intern( FieldName );
                            PayloadField.Result = ResultSig;
                            Store.AddMember( Id, std::move( PayloadField ) );
                        }
                    }
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
                    Frontend::Param &ParamNode = const_cast<Frontend::AstContext &>( Ast ).GetParam( ParamRef );
                    SigTypeId ParamSig;

                    if ( ParamNode.bInstanceVar )
                    {
                        Report( Core::ESeverity::Error, ParamNode.Loc,
                                "instance variable parameter '@" + std::string{ Ast.Text( ParamNode.Name ) } +
                                    "' is not allowed in a free function" );
                    }
                    else if ( not ParamNode.bIsBlock and not ParamNode.DeclType.IsValid() )
                    {
                        Report( Core::ESeverity::Error, ParamNode.Loc,
                                "parameter '" + std::string{ Ast.Text( ParamNode.Name ) } +
                                    "' requires an explicit type annotation" );
                    }
                    else
                    {
                        ParamSig = ResolveTypeExpr( Ast, Store, Scope, Sink, ParamNode.DeclType );
                        if ( not ParamNode.bIsBlock and not ParamSig.IsValid() )
                        {
                            Report( Core::ESeverity::Error, ParamNode.Loc,
                                    "parameter '" + std::string{ Ast.Text( ParamNode.Name ) } +
                                        "' has unresolvable type" );
                        }
                    }

                    Params.PushBack( ParamSig );
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

    void
    ResolveStructLayouts ( std::span<const Frontend::AstContext *const> Units, TypeStore &Store, Core::DiagEngine::Bag *Diags )
    {
        std::vector<NominalId> ActiveStack;
        for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
        {
            const NominalId Id{ static_cast<NominalId::ValueType>( Index ) };
            static_cast<void>( EnsureStructLayout( Units, Store, Id, 0, ActiveStack, Diags ) );
        }
    }

    namespace
    {

        // The `finalize` member spelling and the "does this nominal need
        // finalizing" predicate both used to be redeclared here, verbatim
        // copies of `FinalizeLowering.cpp`'s own. They now have one
        // definition each, in Private/Raii/Ownership.hpp, which this seam and
        // the in-TypeChecker sweeps share — see that header for why the
        // question belongs at the type level.
        using Raii::FinalizeName;
        using Raii::IsFinalizeCandidateNominal;

        // Bounded the same way EnsureStructLayout's own cross-unit recursion
        // is (MaxLayoutDepth) — a genuine by-value cycle cannot exist, but a
        // malformed one must not hang the seam.
        constexpr std::uint32_t MaxFinalizeDepth = 32;

        // Every nominal this type destroys *through*: the written `< Parent`
        // and every `include`d mixin.
        //
        // Read off the AST, exactly as `EnsureStructLayout` reads the same
        // parent link two functions up, and for the identical reason:
        // `NominalType::Super` and `NominalType::Includes` are filled by the
        // *signature* phase, which runs after this seam (Driver.cpp). Asking
        // the store here answers "no parents" for every type in the program —
        // which is precisely how a subclass came to synthesize an empty
        // `finalize` that shadowed its base's real one, statically dispatched,
        // so `ArgumentError` released nothing an `Exception` owns.
        [[nodiscard]] Core::SmallVec<NominalId, 2>
        ParentNominals ( const Frontend::AstContext &Ast, const TypeStore &Store, Frontend::DeclId Decl )
        {
            Core::SmallVec<NominalId, 2> Out;
            if ( const std::optional<NominalId> Super = FieldTypeNominal( Ast, Store, TypeSuperOf( Ast, Decl ) ) )
            {
                Out.PushBack( *Super );
            }
            const Frontend::DeclList *Body = TypeBodyOf( Ast, Decl );
            if ( Body == nullptr )
            {
                return Out;
            }
            for ( const Frontend::DeclId Child : *Body )
            {
                if ( not Child.IsValid() )
                {
                    continue;
                }
                const auto *Included = std::get_if<Frontend::Include>( &Ast.Decl( Child ) );
                if ( Included == nullptr )
                {
                    continue;
                }
                if ( const std::optional<NominalId> Mixin = FieldTypeNominal( Ast, Store, Included->Target ) )
                {
                    Out.PushBack( *Mixin );
                }
            }
            return Out;
        }

        // Ensures `Id`'s own finalize candidacy (already declared by hand, or
        // freshly synthesized as an empty stub here) is *decided* before a
        // sibling recursion asks `IsFinalizeCandidateNominal` on it — the
        // same memoized-by-attachment shape `EnsureStructLayout` already
        // uses for the identical cross-type-order problem (a field may name
        // a type this walk has not reached yet). `Done` memoizes per
        // `NominalId` so a diamond-shaped field graph is visited once.
        void EnsureFinalizeStub ( std::span<Frontend::AstContext *const> Units,
                                  TypeStore &Store,
                                  NominalId Id,
                                  std::vector<bool> &Done,
                                  std::uint32_t Depth )
        {
            if ( not Id.IsValid() or Depth > MaxFinalizeDepth or Id.Value >= Done.size() or Done[Id.Value] )
            {
                return;
            }
            Done[Id.Value] = true;

            // Copied out before the first recursive call below: a sibling
            // recursion `AddMember`s and `Add()`s into arenas this very type
            // also lives in (rules/ast-rewrite.md — never a live reference
            // across a reallocating write).
            const std::uint32_t Unit        = Store.Type( Id ).Unit;
            const Frontend::DeclId TypeDecl = Store.Type( Id ).Decl;

            if ( Unit >= Units.size() or Units[Unit] == nullptr )
            {
                return; // a cache-hit stdlib slot: already synthesized and settled.
            }

            Frontend::AstContext &Ast = *Units[Unit];

            // Settle everything this type destroys through before asking what
            // it owes. The outer `TypeCount()` sweep may reach a subclass
            // first, and a subclass that answered "nothing above me" too early
            // would both mis-report its own triviality and lose the delegation
            // its synthesized destructor owes its base.
            const Core::SmallVec<NominalId, 2> Parents = ParentNominals( Ast, Store, TypeDecl );
            for ( const NominalId Parent : Parents )
            {
                EnsureFinalizeStub( Units, Store, Parent, Done, Depth + 1 );
            }

            // `finalize` is universal, so *having* one says nothing; the
            // question is only ever whether running it does anything. A
            // destructor written by hand always does (the compiler cannot read
            // intent out of a body), an ancestor that is itself non-trivial is
            // reached through this type's own destructor, and a field that
            // needs releasing is released by it.
            const bool bOwnDeclared = Store.OwnMember( Id, FinalizeName ) != nullptr;
            bool bNonTrivial        = bOwnDeclared;
            for ( const NominalId Parent : Parents )
            {
                bNonTrivial = bNonTrivial or IsFinalizeCandidateNominal( Store, Parent );
            }

            // `Mixin`/`Enum` carry no storage of their own and are never
            // synthesized for — but one that *writes* a `finalize` is still
            // non-trivial, and every type including it inherits that, which is
            // why the bit is settled before this gate rather than after.
            const bool bStruct = std::holds_alternative<Frontend::Struct>( Ast.Decl( TypeDecl ) );
            const bool bClass  = std::holds_alternative<Frontend::Class>( Ast.Decl( TypeDecl ) );
            if ( not bStruct and not bClass )
            {
                Store.MutableType( Id ).bTrivialFinalize = not bNonTrivial;
                return;
            }

            // A generic type is *not* excluded. Its field types are indeed
            // unresolved until instantiation, but that is only decisive for a
            // field whose written type *is* a bare parameter: a field written
            // `Array<HashEntry<K,V>>` names a head nominal that declares
            // `finalize` whatever its arguments turn out to be, so candidacy
            // is answerable here and the stub is owed. The field loop below
            // skips bare parameters by name, which is the one case left
            // undecidable (rules/raii-ownership.md).

            Core::SourceRange Loc;
            std::visit(
                [&] ( const auto &N )
                {
                    using T = std::remove_cvref_t<decltype( N )>;
                    if constexpr ( not std::is_same_v<T, std::monostate> )
                    {
                        Loc = N.Loc;
                    }
                },
                Ast.Decl( TypeDecl ) );

            const Frontend::DeclList *BodyPtr = TypeBodyOf( Ast, TypeDecl );
            if ( BodyPtr == nullptr )
            {
                return;
            }
            // Copied out before any recursive `Add()` below — never a
            // reference held across one (rules/ast-rewrite.md).
            const Frontend::DeclList Body = *BodyPtr;

            // Copied out of the store, not held as a reference: the field
            // loop's own recursion calls `AddMember`/`AddSig`, either of
            // which may reallocate (rules/ast-rewrite.md's discipline, same
            // reason `Body` above is a copy).
            std::vector<std::string> ParamNames;
            ParamNames.reserve( Store.Type( Id ).Params.Size() );
            for ( std::size_t P = 0; P < Store.Type( Id ).Params.Size(); ++P )
            {
                ParamNames.emplace_back( Store.Text( Store.Type( Id ).Params[P] ) );
            }

            // A field written as one of *this* type's own generic parameters
            // (`HashEntry<K,V>::key : K`) is the one shape candidacy cannot
            // be decided for here — whether `K` finalizes is a fact of the
            // instantiation, not of the declaration. Skipped by name rather
            // than left to `LookupType` returning nothing by luck: a
            // parameter that happened to share a declared type's spelling
            // would otherwise resolve to that unrelated type.
            const auto bIsOwnParameter = [&] ( const Frontend::TypeId Written )
            {
                if ( not Written.IsValid() )
                {
                    return false;
                }
                const auto *Ref = std::get_if<Frontend::TypeRef>( &Ast.Type( Written ) );
                if ( Ref == nullptr or Ref->Path.Size() != 1 )
                {
                    return false;
                }
                const std::string_view Name = Ast.Text( Ref->Path[0] );
                return std::ranges::find( ParamNames, Name ) != ParamNames.end();
            };

            bool bHasCandidateField = false;
            for ( const Frontend::DeclId Child : Body )
            {
                if ( not Child.IsValid() )
                {
                    continue;
                }
                const auto *Field = std::get_if<Frontend::Field>( &Ast.Decl( Child ) );
                if ( Field == nullptr )
                {
                    continue;
                }
                if ( bIsOwnParameter( Field->DeclType ) )
                {
                    continue;
                }
                const std::optional<NominalId> FieldNominal = FieldTypeNominal( Ast, Store, Field->DeclType );
                if ( not FieldNominal )
                {
                    continue;
                }
                // Settle the field's own type first — it may not have been
                // reached by the outer TypeCount() sweep yet.
                EnsureFinalizeStub( Units, Store, *FieldNominal, Done, Depth + 1 );
                if ( IsFinalizeCandidateNominal( Store, *FieldNominal ) )
                {
                    bHasCandidateField = true;
                    break;
                }
            }
            // A type with nothing to cascade still gets its `finalize` — that
            // is the whole of "universal, like a defaulted destructor". What
            // it does *not* get is a reason for anyone to call it: the
            // triviality bit is what keeps every region's injection set
            // exactly what it was, while making the member itself resolvable
            // on any receiver at all.
            Store.MutableType( Id ).bTrivialFinalize = not( bNonTrivial or bHasCandidateField );

            if ( bOwnDeclared )
            {
                return; // the source wrote one; there is nothing to default.
            }

            // An empty `finalize -> Void` — `ReturnType` stays invalid, which
            // is exactly what a hand-written `-> Void` itself resolves to
            // (`Void` names no declared type; see ResolveTypeExpr's own
            // "unknown name yields an invalid id and no diagnostic").
            // TypeChecker's existing `AppendFieldCascade` fills this Body
            // with the `@field.finalize()` epilogue exactly as it already
            // does for a hand-written `finalize`.
            Frontend::Method Stub;
            Stub.Loc                      = Loc;
            Stub.Name                     = Ast.Strings().Intern( FinalizeName );
            const Frontend::DeclId StubId = Ast.Add( Frontend::DeclNode{ std::move( Stub ) } );

            // Splice into the owning Struct/Class's own Body — copy-out,
            // then write back (rules/ast-rewrite.md); `Ast.Add` above is the
            // only thing that could have invalidated a live reference into
            // the Decl arena, and it already happened before this read.
            if ( bStruct )
            {
                Frontend::Struct Copy = std::get<Frontend::Struct>( Ast.Decl( TypeDecl ) );
                Copy.Body.PushBack( StubId );
                Ast.Decl( TypeDecl ) = Frontend::DeclNode{ std::move( Copy ) };
            }
            else
            {
                Frontend::Class Copy = std::get<Frontend::Class>( Ast.Decl( TypeDecl ) );
                Copy.Body.PushBack( StubId );
                Ast.Decl( TypeDecl ) = Frontend::DeclNode{ std::move( Copy ) };
            }

            // Registered exactly like Phase A's own DeclareMembers would for
            // a hand-written `finalize` — an ordinary zero-parameter Method
            // member, so ResolveUnitSignatures' `Store.MemberByDecl` lookup
            // (which walks the very same `*Decl.Body` this just grew) finds
            // and resolves it like any other, and TypeChecker's
            // `FindOwnFinalizeMethod`/backend emission walk both see it
            // exactly as they would a hand-written one.
            Member Slot;
            Slot.Name = Store.Intern( FinalizeName );
            Slot.Kind = EMemberKind::Method;
            Slot.Unit = Unit;
            Slot.Decl = StubId;
            Store.AddMember( Id, std::move( Slot ) );
        }

    } // namespace

    void SynthesizeFinalizeStubs ( std::span<Frontend::AstContext *const> Units, TypeStore &Store )
    {
        std::vector<bool> Done( Store.TypeCount(), false );
        for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
        {
            EnsureFinalizeStub( Units, Store, NominalId{ static_cast<NominalId::ValueType>( Index ) }, Done, 0 );
        }
    }

} // namespace Sema

} // namespace Volt
