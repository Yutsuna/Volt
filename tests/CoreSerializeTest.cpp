// CoreSerializeTest — round-trip proof for Meta::Serialize/Deserialize
// (Phase 2 of issue #61's stdlib cache). No gtest/Catch2 dependency exists in
// this repo yet, so this follows the project's existing test shape: a small
// standalone executable, registered with add_test, that returns 0 on success
// and a non-zero exit code (with a message on stderr) on the first mismatch.

#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/Core/Support/Arena.hpp"
#include "Volt/Core/Support/SmallVec.hpp"
#include "Volt/Core/Support/StringInterner.hpp"

#include <optional>
#include <print>
#include <string>
#include <variant>
#include <vector>

namespace
{

using namespace Volt;

int Failures = 0;

void Check ( bool bCondition, const char *Label )
{
    if ( not bCondition )
    {
        std::println( stderr, "FAIL: {}", Label );
        ++Failures;
    }
}

enum class EKind : std::uint8_t
{
    Alpha,
    Beta,
    Gamma,
};

struct IntTag
{
};

using IntId = Core::TypedId<IntTag>;

// A plain aggregate exercising every leaf kind Serialize.hpp hand-writes,
// plus the generic ForEachField path (this struct itself has no
// constructors, so it goes through the Reflected overload).
struct Widget
{

    std::int32_t Count = 0;
    bool bEnabled       = false;
    EKind Kind          = EKind::Alpha;
    IntId Id;
    Core::SmallVec<std::int32_t, 2> Small;
    std::optional<std::int32_t> Maybe;
    std::string Name;
    std::vector<std::int32_t> Many;
    std::variant<std::monostate, std::int32_t, std::string> Payload;
};

void RoundTripWidget ()
{
    Widget Original;
    Original.Count   = 42;
    Original.bEnabled = true;
    Original.Kind    = EKind::Gamma;
    Original.Id      = IntId{ 7 };
    Original.Small.PushBack( 1 );
    Original.Small.PushBack( 2 );
    Original.Small.PushBack( 3 ); // forces the SmallVec past its inline capacity of 2
    Original.Maybe = 99;
    Original.Name  = "hello";
    Original.Many  = { 10, 20, 30 };
    Original.Payload = std::string{ "variant-string" };

    Meta::Writer W;
    Meta::Serialize( W, Original );

    Meta::Reader R{ W.Data() };
    Widget Restored;
    const bool bOk = Meta::Deserialize( R, Restored );
    Check( bOk, "Widget deserialize reports success" );
    Check( not R.Failed(), "Widget reader not failed" );

    Check( Restored.Count == Original.Count, "Widget.Count round-trips" );
    Check( Restored.bEnabled == Original.bEnabled, "Widget.bEnabled round-trips" );
    Check( Restored.Kind == Original.Kind, "Widget.Kind round-trips" );
    Check( Restored.Id.Value == Original.Id.Value, "Widget.Id round-trips" );
    Check( Restored.Small.Size() == 3 and Restored.Small[0] == 1 and Restored.Small[1] == 2 and Restored.Small[2] == 3,
           "Widget.Small round-trips" );
    Check( Restored.Maybe.has_value() and *Restored.Maybe == 99, "Widget.Maybe round-trips" );
    Check( Restored.Name == "hello", "Widget.Name round-trips" );
    Check( Restored.Many.size() == 3 and Restored.Many[0] == 10 and Restored.Many[1] == 20 and Restored.Many[2] == 30,
           "Widget.Many round-trips" );
    Check( std::holds_alternative<std::string>( Restored.Payload ) and std::get<std::string>( Restored.Payload ) == "variant-string",
           "Widget.Payload round-trips" );
}

void RoundTripEmptyOptionalAndMonostate ()
{
    Widget Original;
    Original.Maybe   = std::nullopt;
    Original.Payload = std::monostate{};

    Meta::Writer W;
    Meta::Serialize( W, Original );
    Meta::Reader R{ W.Data() };
    Widget Restored;
    Restored.Maybe = 123; // must be cleared by Deserialize, not left stale
    Check( Meta::Deserialize( R, Restored ), "empty-optional widget deserializes" );
    Check( not Restored.Maybe.has_value(), "nullopt round-trips as nullopt" );
    Check( std::holds_alternative<std::monostate>( Restored.Payload ), "monostate round-trips as monostate" );
}

void RoundTripArena ()
{
    Core::Arena<Widget, IntId> Original;
    for ( std::int32_t Index = 0; Index < 5; ++Index )
    {
        Widget W;
        W.Count = Index * 10;
        W.Name  = "item";
        static_cast<void>( Original.Add( std::move( W ) ) );
    }

    Meta::Writer Writer;
    Meta::SerializeArena( Writer, Original );

    Meta::Reader Reader{ Writer.Data() };
    Core::Arena<Widget, IntId> Restored;
    Check( Meta::DeserializeArena( Reader, Restored ), "arena deserialize reports success" );
    Check( Restored.Size() == Original.Size(), "arena size round-trips" );
    for ( std::size_t Index = 0; Index < Original.Size(); ++Index )
    {
        const IntId Id{ static_cast<IntId::ValueType>( Index ) };
        Check( Restored.Get( Id ).Count == Original.Get( Id ).Count, "arena element round-trips at its original Id" );
    }
}

void RoundTripInterner ()
{
    Core::StringInterner Original;
    const Core::Symbol Foo = Original.Intern( "foo" );
    const Core::Symbol Bar = Original.Intern( "bar" );
    const Core::Symbol Baz = Original.Intern( "foo_bar_baz" );

    Meta::Writer Writer;
    Meta::SerializeInterner( Writer, Original );

    Meta::Reader Reader{ Writer.Data() };
    Core::StringInterner Restored; // fresh: only the reserved empty-string slot
    Check( Meta::DeserializeInterner( Reader, Restored ), "interner deserialize reports success" );
    Check( Restored.Size() == Original.Size(), "interner size round-trips" );

    // The whole point of replay-in-order: identical Symbol values, not just
    // identical text at some (possibly different) index.
    Check( Restored.Resolve( Foo ) == "foo", "Symbol Foo resolves identically after replay" );
    Check( Restored.Resolve( Bar ) == "bar", "Symbol Bar resolves identically after replay" );
    Check( Restored.Resolve( Baz ) == "foo_bar_baz", "Symbol Baz resolves identically after replay" );
}

void TruncatedBufferFailsCleanly ()
{
    Widget Original;
    Original.Name = "this string has enough bytes to get truncated mid-field";

    Meta::Writer W;
    Meta::Serialize( W, Original );

    // Chop the buffer short: must fail, never read out of bounds / crash.
    const std::span<const std::byte> Full = std::span<const std::byte>{ W.Data() };
    const std::span<const std::byte> Truncated{ Full.data(), Full.size() / 2 };

    Meta::Reader R{ Truncated };
    Widget Restored;
    Check( not Meta::Deserialize( R, Restored ), "truncated buffer is reported as a failed deserialize" );
    Check( R.Failed(), "truncated reader ends in the Failed state" );
}

} // namespace

int main ()
{
    RoundTripWidget();
    RoundTripEmptyOptionalAndMonostate();
    RoundTripArena();
    RoundTripInterner();
    TruncatedBufferFailsCleanly();

    if ( Failures == 0 )
    {
        std::println( "CoreSerializeTest: all checks passed" );
        return 0;
    }
    std::println( stderr, "CoreSerializeTest: {} check(s) failed", Failures );
    return 1;
}
