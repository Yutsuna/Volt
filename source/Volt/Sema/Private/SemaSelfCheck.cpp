// Compile-time + smoke check for the Sema skeleton: the MemoryLayout variant,
// the TypeStore arena, and the manifest-driven pass registry. Exercised under
// -Werror so the header-only machinery is actually instantiated.

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
            Pair.Fields.PushBack( FieldLayout{ Interner.Intern( "head" ), Word } );
            Pair.Fields.PushBack( FieldLayout{ Interner.Intern( "next" ), Ptr } );
            const LayoutId Node = Store.AddAggregate( std::move( Pair ) );

            Store.Bind( Interner.Intern( "Node" ), Node );
            if ( Store.Lookup( Interner.Intern( "Node" ) ) != Node || Store.Size() != 3 )
            {
                return false;
            }
            if ( KindOf( Store.Get( Ptr ) ) != LayoutKind::Pointer )
            {
                return false;
            }

            // The registry must expose the manifest passes in ascending
            // Order regardless of their listed order.
            const std::span<const PassInfo> Registry = PassRegistry();
            if ( Registry.size() != 3 )
            {
                return false;
            }
            for ( std::size_t I = 1; I < Registry.size(); ++I )
            {
                if ( Registry[I - 1].Order > Registry[I].Order )
                {
                    return false;
                }
            }

            Frontend::AstContext Ast{ Interner, File };
            Core::DiagEngine Engine;
            Core::DiagEngine::Bag Bag = Engine.MakeBag();
            PassStats Stats;
            PassContext Context{ Ast, Store, Bag, Stats };
            const std::size_t Ran = RunPasses( Context );

            return Ran == Registry.size() && Bag.Errors() == 0;
        }

    } // namespace

} // namespace Sema

} // namespace Volt
