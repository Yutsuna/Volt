// Compile-time + smoke check for MiddleEnd::TypeSystem: MemoryLayout variant & TypeStore arena.

#ifndef DEBUG_NO_STATIC_ASSERT

    #include "Volt/Core/Support/StringInterner.hpp"
    #include "Volt/MiddleEnd/TypeSystem/MemoryLayout.hpp"
    #include "Volt/MiddleEnd/TypeSystem/TypeStore.hpp"

    #include <cstddef>

namespace Volt
{

namespace MiddleEnd
{

    namespace TypeSystem
    {

        namespace
        {

            static_assert( std::variant_size_v<LayoutNode> == 4 );
            static_assert( KindOf( LayoutNode{ Primitive{} } ) == LayoutKind::Primitive );
            static_assert( KindOf( LayoutNode{ Pointer{} } ) == LayoutKind::Pointer );

            [[maybe_unused]] bool RunTypeSystemSmokeTest ( ::Volt::Core::StringInterner &Interner )
            {
                TypeStore Store;

                const LayoutId Word = Store.AddPrimitive( Interner.Intern( "i32" ), 32 );
                const LayoutId Ptr  = Store.AddPointer( Word );

                Aggregate Pair;
                Pair.Fields.PushBack( FieldLayout{ .Name = Interner.Intern( "head" ), .Type = Word } );
                Pair.Fields.PushBack( FieldLayout{ .Name = Interner.Intern( "next" ), .Type = Ptr } );
                const LayoutId Node = Store.AddAggregate( Pair );

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

                if ( not Store.BindNodeKind( "IntLiteral", Named ) or Store.LookupNodeKind( "IntLiteral" ) != Named )
                {
                    return false;
                }
                const NominalId Other = Store.DeclareType( "Other", 0, Frontend::DeclId{} );
                if ( Store.BindNodeKind( "IntLiteral", Other ) )
                {
                    return false;
                }

                return true;
            }

        } // namespace

    } // namespace TypeSystem

} // namespace MiddleEnd

} // namespace Volt

#endif // DEBUG_NO_STATIC_ASSERT
