// MacroEngine.cpp — the seam step that turns `macro def` into a real method.
//
// MacroEval.cpp answers "what does this macro body produce"; this file answers
// "for whom, and where does the result go". Those are separate questions
// because the second one is entirely about the *store*: a generated method
// exists for the rest of the compiler only once it is a Member of its target
// type, and only the serial seam may write one (MacroEngine.hpp has the full
// argument).
//
// Arena discipline throughout (rules/ast-rewrite.md): every node and every
// store record is copied out before anything is added, since `Ast.Add` and
// `Store.AddMember` both reallocate under a reference that was taken before
// them. The precedent this file follows step for step is
// SynthesizeFinalizeStubs (TypeBinder.cpp), which grafts a synthesized
// `finalize` the same way.

#include "Volt/MiddleEnd/ConstEval/MacroEngine.hpp"

#include "MacroEval.hpp"

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Frontend/AST/AstClone.hpp"
#include "Volt/Frontend/AST/AstQuery.hpp"
#include "Volt/Frontend/AST/Decl.hpp"
#include "Volt/Frontend/AST/Type.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{

using namespace Volt;
using namespace Volt::Frontend;
using namespace Volt::MiddleEnd::ConstEval;

namespace TypeSystem = Volt::MiddleEnd::TypeSystem;

// `Struct` / `Class` / `Mixin` / `Enum` / `Module` all collapse to one shape
// here — a list of declarations — so recovering "what is this thing's body"
// from a DeclId has to re-open the variant, exactly as TypeBinder's own
// TypeBodyOf does.
[[nodiscard]] const DeclList *BodyOf ( const AstContext &Ast, DeclId Decl )
{
    if ( not Decl.IsValid() )
    {
        return nullptr;
    }
    return std::visit( Meta::Overloaded{ [] ( const Struct &Node ) -> const DeclList * { return &Node.Body; },
                                         [] ( const Class &Node ) -> const DeclList * { return &Node.Body; },
                                         [] ( const Mixin &Node ) -> const DeclList * { return &Node.Body; },
                                         [] ( const Enum &Node ) -> const DeclList * { return &Node.Body; },
                                         [] ( const Module &Node ) -> const DeclList * { return &Node.Body; },
                                         [] ( const auto & ) -> const DeclList * { return nullptr; } },
                       Ast.Decl( Decl ) );
}

/// Replace a type's body wholesale. The caller has already built the new list;
/// this is the write-back half of the copy-out discipline.
void SetBody ( AstContext &Ast, DeclId Decl, DeclList Body )
{
    std::visit( Meta::Overloaded{ [&] ( Struct &Node ) { Node.Body = std::move( Body ); }, [&] ( Class &Node )
                                  { Node.Body = std::move( Body ); }, [&] ( Mixin &Node ) { Node.Body = std::move( Body ); },
                                  [&] ( Enum &Node ) { Node.Body = std::move( Body ); },
                                  [&] ( Module &Node ) { Node.Body = std::move( Body ); }, [] ( auto & ) {} },
                Ast.Decl( Decl ) );
}

// The nominal a written type names, dropping any generic arguments — the same
// one-line resolution TypeBinder's FieldTypeNominal performs, and all an
// `include` needs to be followed.
[[nodiscard]] std::optional<TypeSystem::NominalId>
NominalOf ( const AstContext &Ast, const TypeSystem::TypeStore &Store, TypeId Id )
{
    if ( not Id.IsValid() )
    {
        return std::nullopt;
    }
    const auto *Ref = std::get_if<TypeRef>( &Ast.Type( Id ) );
    if ( Ref == nullptr or Ref->Path.IsEmpty() )
    {
        return std::nullopt;
    }
    return Store.LookupType( Ast.Text( Ref->Path[Ref->Path.Size() - 1] ) );
}

/// One macro body waiting to be evaluated for one target type, named by the
/// unit it lives in rather than by a pointer: the seam adds nodes to these very
/// arenas between collection and evaluation.
struct MacroSite
{

    std::uint32_t Unit = 0;
    DeclId Decl;
};

class Engine
{

public:

    Engine ( std::span<AstContext *const> InUnits,
             TypeSystem::TypeStore &InStore,
             const ::Volt::Core::SourceManager &InSources,
             ::Volt::Core::DiagEngine::Bag &InDiags )
        : Units( InUnits ), Store( InStore ), Sources( InSources ), Diags( InDiags )
    {
    }

    void Run ()
    {
        for ( std::size_t Index = 0; Index < Store.TypeCount(); ++Index )
        {
            ExpandType( TypeSystem::NominalId{ static_cast<TypeSystem::NominalId::ValueType>( Index ) } );
        }

        // After every generation, so a `macro do` sees the same world whichever
        // file it sits in, and in arena order — which is parse order, which is
        // file order, which is what makes compile-time output reproducible.
        // A parallel pass could never promise that, and is half the reason this
        // step is at the seam at all.
        for ( std::size_t Index = 0; Index < Units.size(); ++Index )
        {
            RunMacroBlocks( static_cast<std::uint32_t>( Index ) );
        }
        for ( AstContext *Unit : Units )
        {
            if ( Unit != nullptr )
            {
                StripMacros( *Unit );
            }
        }
    }

private:

    // --- Generation --------------------------------------------------------

    void ExpandType ( TypeSystem::NominalId Target )
    {
        // Copied out before the first Add / AddMember below: both reallocate,
        // and the store record is what they reallocate (rules/ast-rewrite.md).
        const std::uint32_t Unit     = Store.Type( Target ).Unit;
        const DeclId TypeDecl        = Store.Type( Target ).Decl;
        const bool bGeneric          = not Store.Type( Target ).Params.IsEmpty();
        const std::string TargetName = std::string( Store.Text( Store.Type( Target ).Name ) );

        AstContext *TargetAst = UnitAt( Unit );
        if ( TargetAst == nullptr or Store.IsMixin( Target ) )
        {
            // A mixin is never a target: `self` in a macro means the concrete
            // type that includes it, and a cache-served unit was expanded when
            // its cache was written.
            return;
        }

        const std::vector<MacroSite> Sites = CollectMacros( *TargetAst, TypeDecl, Unit );
        if ( Sites.empty() )
        {
            return;
        }
        if ( bGeneric )
        {
            // A field of a generic type is written in terms of that type's own
            // parameters, and nothing can answer what one of them *is* until an
            // instantiation asks — which happens long after this seam. Refused
            // rather than half-answered.
            Diags.Error( LocOf( TargetAst->Decl( TypeDecl ) ),
                         "'" + TargetName + "' is generic, and a macro cannot be expanded for it" );
            return;
        }

        for ( const MacroSite &Site : Sites )
        {
            Generate( Target, Unit, TypeDecl, TargetName, Site );
        }
    }

    /// Every `macro def` that applies to a type: its own, then those of each
    /// mixin it includes. The `include` list is read off the AST because
    /// NominalType::Includes is filled by the signature phase, which runs after
    /// this one — the same reason ParentNominals reads it there.
    [[nodiscard]] std::vector<MacroSite> CollectMacros ( const AstContext &Ast, DeclId TypeDecl, std::uint32_t OwnUnit )
    {
        std::vector<MacroSite> Sites;
        const DeclList *BodyPtr = BodyOf( Ast, TypeDecl );
        if ( BodyPtr == nullptr )
        {
            return Sites;
        }
        const DeclList Body = *BodyPtr;

        for ( const DeclId Child : Body )
        {
            if ( Child.IsValid() and std::holds_alternative<MacroDef>( Ast.Decl( Child ) ) )
            {
                Sites.push_back( MacroSite{ .Unit = OwnUnit, .Decl = Child } );
            }
        }

        for ( const DeclId Child : Body )
        {
            if ( not Child.IsValid() )
            {
                continue;
            }
            const auto *Included = std::get_if<Include>( &Ast.Decl( Child ) );
            if ( Included == nullptr )
            {
                continue;
            }
            const std::optional<TypeSystem::NominalId> Mixin = NominalOf( Ast, Store, Included->Target );
            if ( not Mixin )
            {
                continue;
            }
            const std::uint32_t MixinUnit = Store.Type( *Mixin ).Unit;
            const DeclId MixinDecl        = Store.Type( *Mixin ).Decl;
            const AstContext *MixinAst    = UnitAt( MixinUnit );
            const DeclList *MixinBody     = MixinAst == nullptr ? nullptr : BodyOf( *MixinAst, MixinDecl );
            if ( MixinBody == nullptr )
            {
                continue;
            }
            for ( const DeclId MixinChild : *MixinBody )
            {
                if ( MixinChild.IsValid() and std::holds_alternative<MacroDef>( MixinAst->Decl( MixinChild ) ) )
                {
                    Sites.push_back( MacroSite{ .Unit = MixinUnit, .Decl = MixinChild } );
                }
            }
        }
        return Sites;
    }

    void Generate ( TypeSystem::NominalId Target,
                    std::uint32_t TargetUnit,
                    DeclId TypeDecl,
                    const std::string &TargetName,
                    const MacroSite &Site )
    {
        AstContext *SourceAst = UnitAt( Site.Unit );
        AstContext *TargetAst = UnitAt( TargetUnit );
        if ( SourceAst == nullptr or TargetAst == nullptr )
        {
            return;
        }

        const MacroDef Macro = std::get<MacroDef>( SourceAst->Decl( Site.Decl ) );
        const std::string Name( SourceAst->Text( Macro.Name ) );

        // Silently shadowing a member somebody wrote would make a generated
        // method impossible to find in the source it appears to come from.
        if ( Store.OwnMember( Target, Name ) != nullptr )
        {
            Diags.Error( Macro.Loc, "macro '" + Name + "' generates a member '" + TargetName + "' already declares" );
            return;
        }

        MacroEnv Env{ .Source   = *SourceAst,
                      .Target   = *TargetAst,
                      .Store    = Store,
                      .Units    = Units,
                      .SelfType = Target,
                      .Site     = SiteOf( *SourceAst, Macro.Loc, Name ),
                      .Diags    = Diags,
                      .WorkDir  = DirectoryOf( *SourceAst ),
                      .Depth    = 0 };

        StmtList Emitted;
        // A macro that declares a return type ends on the method's result, so a
        // compile-time value in tail position materialises there instead of
        // vanishing like every other executed statement.
        EvalMacroBody( Env, Macro.Body, Emitted, Macro.ReturnType.IsValid() );

        Method Generated;
        Generated.Loc  = Macro.Loc;
        Generated.Name = TargetAst->Strings().Intern( Name );
        for ( const ParamId Param : Macro.Params )
        {
            Generated.Params.PushBack( CloneParam( *SourceAst, *TargetAst, Param ) );
        }
        Generated.ReturnType = CloneType( *SourceAst, *TargetAst, Macro.ReturnType );
        Generated.Body       = std::move( Emitted );
        Generated.bSelf      = Macro.bSelf;
        Generated.Visibility = Macro.Visibility;

        const DeclId GeneratedId = TargetAst->Add( DeclNode{ std::move( Generated ) } );

        // Splice into the target type's own Body — copy out, then write back.
        // The Add above is the last thing that could have invalidated a
        // reference into the Decl arena, and it has already happened.
        const DeclList *BodyPtr = BodyOf( *TargetAst, TypeDecl );
        if ( BodyPtr == nullptr )
        {
            return;
        }
        DeclList Body = *BodyPtr;
        Body.PushBack( GeneratedId );
        SetBody( *TargetAst, TypeDecl, std::move( Body ) );

        // Registered exactly as Phase A's DeclareMembers would have for a
        // hand-written method: name, kind, where it lives. The signature loop
        // that runs next walks this very Body, finds the member by its DeclId
        // and fills in everything else.
        TypeSystem::Member Slot;
        Slot.Name       = Store.Intern( Name );
        Slot.Kind       = TypeSystem::EMemberKind::Method;
        Slot.Unit       = TargetUnit;
        Slot.Decl       = GeneratedId;
        Slot.bSelf      = Macro.bSelf;
        Slot.Visibility = Macro.Visibility;
        Store.AddMember( Target, std::move( Slot ) );
    }

    // --- `macro do` --------------------------------------------------------

    void RunMacroBlocks ( std::uint32_t Unit )
    {
        AstContext *Ast = UnitAt( Unit );
        if ( Ast == nullptr )
        {
            return;
        }

        // The Decl arena in index order is parse order: a block declared in a
        // module runs after the blocks written above it, and before those
        // below, whatever nests it.
        for ( std::size_t Index = 0; Index < Ast->DeclCount(); ++Index )
        {
            const DeclId Id{ static_cast<DeclId::ValueType>( Index ) };
            const auto *Block = std::get_if<MacroBlock>( &Ast->Decl( Id ) );
            if ( Block == nullptr )
            {
                continue;
            }
            const MacroBlock Node = *Block;

            MacroEnv Env{ .Source   = *Ast,
                          .Target   = *Ast,
                          .Store    = Store,
                          .Units    = Units,
                          .SelfType = TypeSystem::NominalId{},
                          .Site     = SiteOf( *Ast, Node.Loc, {} ),
                          .Diags    = Diags,
                          .WorkDir  = DirectoryOf( *Ast ),
                          .Depth    = 0 };

            StmtList Emitted;
            EvalMacroBody( Env, Node.Body, Emitted, false );
            if ( not Emitted.IsEmpty() )
            {
                // A `macro do` emits nothing by definition, so anything it
                // produced is code that would be silently dropped — worth
                // saying rather than losing.
                Diags.Warning( Node.Loc, "this 'macro do' contains code that is not compile-time; it is not emitted" );
            }
        }
    }

    // --- Retiring the macros ------------------------------------------------

    // Nothing after this seam has any business seeing a macro: the methods are
    // generated, the blocks have run. The nodes stay in the arena — arenas only
    // grow, and MetadataExprs still reaches them there to keep their untyped,
    // unlowered insides out of AstInvariant's census — but no declaration list
    // names them any more.
    void StripMacros ( AstContext &Ast )
    {
        for ( std::size_t Index = 0; Index < Ast.DeclCount(); ++Index )
        {
            const DeclId Id{ static_cast<DeclId::ValueType>( Index ) };
            const DeclList *BodyPtr = BodyOf( Ast, Id );
            if ( BodyPtr == nullptr )
            {
                continue;
            }
            DeclList Kept;
            bool bDropped = false;
            for ( const DeclId Child : *BodyPtr )
            {
                if ( IsMacro( Ast, Child ) )
                {
                    bDropped = true;
                    continue;
                }
                Kept.PushBack( Child );
            }
            if ( bDropped )
            {
                SetBody( Ast, Id, std::move( Kept ) );
            }
        }

        std::vector<DeclId> Kept;
        Kept.reserve( Ast.TopDecls.size() );
        for ( const DeclId Child : Ast.TopDecls )
        {
            if ( not IsMacro( Ast, Child ) )
            {
                Kept.push_back( Child );
            }
        }
        Ast.TopDecls = std::move( Kept );
    }

    [[nodiscard]] static bool IsMacro ( const AstContext &Ast, DeclId Id )
    {
        if ( not Id.IsValid() )
        {
            return false;
        }
        const DeclNode &Node = Ast.Decl( Id );
        return std::holds_alternative<MacroDef>( Node ) or std::holds_alternative<MacroBlock>( Node );
    }

    // --- Where a macro body thinks it is ------------------------------------

    /// `__FILE__` & co answer for the file the macro body is *written* in, not
    /// the one it generates into: the nodes carry that file's SourceRange, and
    /// a mixin author reasoning about `__DIR__` means their own directory.
    [[nodiscard]] MagicSite SiteOf ( const AstContext &Ast, ::Volt::Core::SourceRange Loc, std::string_view Function )
    {
        if ( not Sources.IsValidFile( Ast.FileId() ) )
        {
            return MagicSite{ .Path = {}, .Dir = {}, .Function = Function, .Line = 1, .Column = 1 };
        }
        const std::string_view Path          = Sources.PathOf( Ast.FileId() );
        const std::size_t Slash              = Path.find_last_of( '/' );
        const ::Volt::Core::LineColumn Where = Sources.Resolve( Ast.FileId(), Loc.Begin );

        return MagicSite{ .Path     = Path,
                          .Dir      = Slash == std::string_view::npos ? std::string_view{} : Path.substr( 0, Slash ),
                          .Function = Function,
                          .Line     = Where.Line,
                          .Column   = Where.Column };
    }

    /// A command runs where its file lives, so a relative path in one means
    /// what the source says — the same directory `__DIR__` reports.
    [[nodiscard]] std::string DirectoryOf ( const AstContext &Ast )
    {
        if ( not Sources.IsValidFile( Ast.FileId() ) )
        {
            return {};
        }
        const std::string_view Path = Sources.PathOf( Ast.FileId() );
        const std::size_t Slash     = Path.find_last_of( '/' );
        return Slash == std::string_view::npos ? std::string{} : std::string( Path.substr( 0, Slash ) );
    }

    [[nodiscard]] AstContext *UnitAt ( std::uint32_t Unit ) const
    {
        return Unit < Units.size() ? Units[Unit] : nullptr;
    }

    std::span<AstContext *const> Units;
    TypeSystem::TypeStore &Store;
    const ::Volt::Core::SourceManager &Sources;
    ::Volt::Core::DiagEngine::Bag &Diags;
};

} // namespace

namespace Volt::MiddleEnd::ConstEval
{

void ExpandTypeMacros ( std::span<Frontend::AstContext *const> Units,
                        TypeSystem::TypeStore &Store,
                        const ::Volt::Core::SourceManager &Sources,
                        ::Volt::Core::DiagEngine::Bag &Diags )
{
    Engine( Units, Store, Sources, Diags ).Run();
}

} // namespace Volt::MiddleEnd::ConstEval
