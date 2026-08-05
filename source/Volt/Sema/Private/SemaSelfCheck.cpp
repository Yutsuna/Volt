// Compile-time + smoke check for the Sema skeleton: the MemoryLayout variant,
// the TypeStore arena, and the manifest-driven pass registry. Exercised under
// -Werror so the header-only machinery is actually instantiated.

#ifndef DEBUG_NO_STATIC_ASSERT

    #include "Volt/Core/Support/StringInterner.hpp"
    #include "Volt/Frontend/AST/AstContext.hpp"
    #include "Volt/Sema/Layout/MemoryLayout.hpp"
    #include "Volt/Sema/Layout/TypeStore.hpp"
    #include "Volt/Sema/Pass.hpp"

    #include <cstddef>

namespace Volt
{

namespace Sema
{

    namespace
    {

        static_assert( std::variant_size_v<LayoutNode> == 4 );
        static_assert( KindOf( LayoutNode{ Primitive{} } ) == LayoutKind::Primitive );
        static_assert( KindOf( LayoutNode{ Pointer{} } ) == LayoutKind::Pointer );

        [[maybe_unused]] bool RunSmokeTest ( Core::StringInterner &Interner, Core::FileId File )
        {
            TypeStore Store;

            // A primitive described only by an opaque spelling — no Volt type
            // name ever appears in C++ (zero-hardcode guard).
            const LayoutId Word = Store.AddPrimitive( Interner.Intern( "i32" ), 32 );
            const LayoutId Ptr  = Store.AddPointer( Word );

            Aggregate Pair;
            Pair.Fields.PushBack( FieldLayout{ .Name = Interner.Intern( "head" ), .Type = Word } );
            Pair.Fields.PushBack( FieldLayout{ .Name = Interner.Intern( "next" ), .Type = Ptr } );
            const LayoutId Node = Store.AddAggregate( Pair );

            // A type is an identity first; the layout is attached to it.
            const NominalId Named = Store.DeclareType( "Node", 0, Frontend::DeclId{} );
            Store.AttachLayout( Named, Node );
            if ( Store.LookupType( "Node" ) != Named or Store.Size() != 3 )
            {
                return false;
            }
            if ( Store.Type( Named ).Layout != Node or KindOf( Store.Get( Ptr ) ) != LayoutKind::Pointer )
            {
                return false;
            }

            // A literal kind resolves to the *type* that claimed it, and a
            // second claimant must be refused rather than silently winning.
            if ( not Store.BindNodeKind( "IntLiteral", Named ) or Store.LookupNodeKind( "IntLiteral" ) != Named )
            {
                return false;
            }
            const NominalId Other = Store.DeclareType( "Other", 0, Frontend::DeclId{} );
            if ( Store.BindNodeKind( "IntLiteral", Other ) )
            {
                return false;
            }

            // The registry must expose the manifest passes in ascending
            // Order regardless of their listed order.
            const std::span<const PassInfo> Registry = PassRegistry();
            if ( Registry.empty() )
            {
                return false;
            }
            for ( std::size_t Idx = 1; Idx < Registry.size(); ++Idx )
            {
                if ( Registry[Idx - 1].Order > Registry[Idx].Order )
                {
                    return false;
                }
            }

            Frontend::AstContext Ast{ Interner, File };
            Core::DiagEngine Engine;
            Core::DiagEngine::Bag Bag = Volt::Core::DiagEngine::MakeBag();
            PassStats Stats;
            UnitTypes Values;
            ScopeTable Scopes;
            SynthesizedFunctions Synth;
            PassContext Context{
                .Ast = Ast, .Types = Store, .Values = Values, .Scopes = Scopes, .Diags = Bag, .Stats = Stats, .Synth = Synth };
            const std::size_t Ran = RunPasses( Context );

            return Ran == Registry.size() and Bag.Errors() == 0;
        }

    } // namespace

} // namespace Sema

} // namespace Volt

#endif // DEBUG_NO_STATIC_ASSERT
