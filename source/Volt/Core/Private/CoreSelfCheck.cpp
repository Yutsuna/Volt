/**
 * @file CoreSelfCheck.cpp
 * @brief Compile-time smoke test for the header-only Support/Meta foundation.
 * @details Templates are not checked until instantiated,
 *          So this TU instantiates the core utilities
 *          and asserts their invariants under -Werror.
 */

#include "Volt/Core/Meta/Overloaded.hpp"
#include "Volt/Core/Meta/Reflect.hpp"
#include "Volt/Core/Meta/TypeList.hpp"
#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/Id.hpp"
#include "Volt/Core/Support/Result.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Core/Support/Span.hpp"
#include "Volt/Core/Support/StringInterner.hpp"

#include <string>
#include <variant>

namespace Volt
{

namespace Core
{

    namespace
    {

        struct NodeTag
        {
        };

        using NodeId = TypedId<NodeTag>;

        static_assert( !NodeId{}.IsValid() );
        static_assert( NodeId{ 3 }.IsValid() );

        struct Sample
        {

            int A    = 0;
            double B = 0.0;
        };

        static_assert( Meta::Reflected<Sample> );
        static_assert( Meta::FieldCount<Sample>() == 2 );

        using Numbers = Meta::TypeList<int, float, double>;
        static_assert( Numbers::Size == 3 );
        static_assert( Meta::IndexOfV<double, Numbers> == 2 );
        static_assert( std::is_same_v<Meta::AsVariantT<Numbers>, std::variant<int, float, double>> );

        [[maybe_unused]] bool RunSmokeTest ()
        {
            Arena<std::string, NodeId> Strings;
            const NodeId First  = Strings.Add( "alpha" );
            const NodeId Second = Strings.Add( "beta" );
            if ( Strings.Get( First ) != "alpha" or Strings.Get( Second ) != "beta" )
            {
                return false;
            }

            SmallVec<int, 2> Vec;
            for ( int Value = 0; Value < 8; ++Value )
            {
                Vec.PushBack( Value );
            }
            int Sum = 0;
            for ( const int Value : Vec )
            {
                Sum += Value;
            }
            if ( Sum != 28 or Vec.Size() != 8 )
            {
                return false;
            }

            StringInterner Interner;
            const Symbol Foo  = Interner.Intern( "foo" );
            const Symbol Foo2 = Interner.Intern( "foo" );
            const Symbol Bar  = Interner.Intern( "bar" );
            if ( Foo != Foo2 or Foo == Bar or Interner.Resolve( Foo ) != "foo" )
            {
                return false;
            }

            Result<int, std::string> Ok  = 42;
            Result<int, std::string> Err = Fail( std::string{ "boom" } );
            if ( !Ok.has_value() or Err.has_value() )
            {
                return false;
            }

            const std::variant<int, double> Variant = 2.5;
            const bool bIsDouble =
                std::visit( Meta::Overloaded{ [] ( int ) { return false; }, [] ( double ) { return true; } }, Variant );

            int Reflected = 0;
            Sample S{ .A = 7, .B = 1.5 };
            Meta::ForEachField( S, [&] ( const char *, auto & ) { ++Reflected; } );

            return bIsDouble and Reflected == 2;
        }

    } // namespace

} // namespace Core

} // namespace Volt
