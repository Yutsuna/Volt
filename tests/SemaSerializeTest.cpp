// SemaSerializeTest — round-trip proof for the Sema-side cache serializers
// built on Meta::Serialize (issue #61, Phase 2b): TypeStore, UnitCallees'
// two-phase Decl fixup, ScopeTable's UseIndex fixup, UnitTypes' Dedup
// rebuild, and InterfaceRegistry's Publish-replay. Same standalone-executable
// shape as CoreSerializeTest — no gtest/Catch2 dependency in this repo.

#include "Volt/Core/Meta/Serialize.hpp"
#include "Volt/Sema/Layout/CalleeMap.hpp"
#include "Volt/Sema/Layout/SemaType.hpp"
#include "Volt/Sema/Layout/TypeStore.hpp"
#include "Volt/Sema/Link/InterfaceRegistry.hpp"
#include "Volt/Sema/Scope/ScopeTable.hpp"

#include <print>

namespace
{

using namespace Volt;
using namespace Volt::Sema;

int Failures = 0;

void Check ( bool bCondition, const char *Label )
{
    if ( not bCondition )
    {
        std::println( stderr, "FAIL: {}", Label );
        ++Failures;
    }
}

// --- TypeStore -----------------------------------------------------------

void RoundTripTypeStore ()
{
    TypeStore Original;

    const NominalId Int32Id = Original.DeclareType( "Int32", 0, Frontend::DeclId{ 1 } );
    Original.AttachLayout( Int32Id, Original.AddPrimitive( Original.Intern( "i32" ), 32 ) );

    const NominalId ArrayId = Original.DeclareType( "Array", 0, Frontend::DeclId{ 2 } );
    Original.SetParams( ArrayId, { Original.Intern( "T" ) } );

    Member Push;
    Push.Name       = Original.Intern( "push" );
    Push.Kind       = EMemberKind::Method;
    Push.Unit       = 0;
    Push.Decl       = Frontend::DeclId{ 3 };
    Push.MinParams  = 1;
    Original.AddMember( ArrayId, Push );

    Original.DeclareModule( "MathUtils" );
    Member Square = *Original.DeclareFunction( "square", 0, Frontend::DeclId{ 4 } );
    static_cast<void>( Square );
    Check( Original.SetExceptionRoot( Int32Id ), "ExceptionRoot sets cleanly on a fresh store" );

    Meta::Writer W;
    Original.SerializeCache( W );

    Meta::Reader R{ W.Data() };
    TypeStore Restored;
    Check( Restored.DeserializeCache( R ), "TypeStore deserialize reports success" );
    Check( not R.Failed(), "TypeStore reader not failed" );

    Check( Restored.TypeCount() == Original.TypeCount(), "TypeStore.TypeCount round-trips" );
    Check( Restored.LookupType( "Int32" ) == Int32Id, "Int32 keeps its original NominalId" );
    Check( Restored.LookupType( "Array" ) == ArrayId, "Array keeps its original NominalId" );
    Check( Restored.Type( ArrayId ).Params.Size() == 1, "Array's generic parameter round-trips" );
    Check( Restored.OwnMember( ArrayId, "push" ) != nullptr, "Array#push round-trips as a member" );
    Check( Restored.IsModule( "MathUtils" ), "module registration round-trips" );
    Check( Restored.LookupFunction( "square" ) != nullptr, "free function round-trips" );
    Check( Restored.GetExceptionRoot() == std::optional<NominalId>{ Int32Id }, "ExceptionRoot round-trips" );
    Check( Restored.Type( Int32Id ).Layout.IsValid(), "attached layout round-trips" );

    const Member *FoundMember = Restored.FindMemberByUnitDecl( 0, Frontend::DeclId{ 3 } );
    Check( FoundMember != nullptr and Restored.Text( FoundMember->Name ) == "push", "FindMemberByUnitDecl finds a type member" );
    const Member *FoundFunc = Restored.FindMemberByUnitDecl( 0, Frontend::DeclId{ 4 } );
    Check( FoundFunc != nullptr and Restored.Text( FoundFunc->Name ) == "square", "FindMemberByUnitDecl finds a free function" );
}

// --- UnitCallees: the two-phase Decl fixup --------------------------------

void RoundTripCalleeMapFixup ()
{
    TypeStore Original;
    const NominalId Owner = Original.DeclareType( "Array", 0, Frontend::DeclId{ 10 } );
    Member Push;
    Push.Name = Original.Intern( "push" );
    Push.Unit = 0;
    Push.Decl = Frontend::DeclId{ 11 };
    Original.AddMember( Owner, Push );
    const Member *Decl = Original.MemberByDecl( Owner, 0, Frontend::DeclId{ 11 } );

    UnitCallees Callees;
    CalleeEntry ResolvedEntry;
    ResolvedEntry.Decl = Decl;
    Callees.Set( Frontend::ExprId{ 5 }, ResolvedEntry );

    CalleeEntry UnresolvedEntry; // e.g. a primitive op, deliberately unresolved
    Callees.Set( Frontend::ExprId{ 6 }, UnresolvedEntry );

    Meta::Writer StoreW;
    Original.SerializeCache( StoreW );
    Meta::Reader StoreR{ StoreW.Data() };
    TypeStore Restored;
    Check( Restored.DeserializeCache( StoreR ), "fixup-support TypeStore deserializes" );

    Meta::Writer CalleesW;
    Callees.SerializeCache( CalleesW );
    Meta::Reader CalleesR{ CalleesW.Data() };
    UnitCallees RestoredCallees;
    Check( RestoredCallees.DeserializeCache( CalleesR ), "UnitCallees deserialize reports success" );

    // Before the fixup pass, Decl must not be left dangling into the
    // now-destroyed `Original` store.
    RestoredCallees.FixupDecls( Restored );

    const CalleeEntry *Resolved = RestoredCallees.Get( Frontend::ExprId{ 5 } );
    Check( Resolved != nullptr and Resolved->Decl != nullptr, "resolved callee's Decl survives the round-trip" );
    Check( Resolved != nullptr and Resolved->Decl != nullptr and Restored.Text( Resolved->Decl->Name ) == "push",
           "fixed-up Decl points at the right member" );

    const CalleeEntry *Unresolved = RestoredCallees.Get( Frontend::ExprId{ 6 } );
    Check( Unresolved != nullptr and Unresolved->Decl == nullptr, "a deliberately-null Decl stays null after fixup" );
}

// --- ScopeTable: the UseIndex fixup ---------------------------------------

void RoundTripScopeTable ()
{
    ScopeTable Original;
    const ScopeId Root = Original.PushScope( ScopeId{}, EScopeKind::Unit );
    const Symbol Name  = Symbol{ 42 }; // any interned-looking value; this table never resolves text itself
    Check( Original.Declare( Root, Name, Frontend::StmtId{ 7 } ), "declaring a fresh binding succeeds" );
    Original.BindUse( Frontend::ExprId{ 1 }, *Original.Resolve( Root, Name ) );

    Meta::Writer W;
    Original.SerializeCache( W );
    Meta::Reader R{ W.Data() };
    ScopeTable Restored;
    Check( Restored.DeserializeCache( R ), "ScopeTable deserialize reports success" );
    Check( not R.Failed(), "ScopeTable reader not failed" );

    const Binding *Use = Restored.BindingOf( Frontend::ExprId{ 1 } );
    Check( Use != nullptr, "UseIndex fixup resolves a live Binding pointer" );
    Check( Use != nullptr and Use->Name == Name, "resolved Binding carries the original Name" );
    Check( Use != nullptr and Restored.UseCountOf( Use->Site ) == 1, "UseCounts round-trips" );
}

// --- UnitTypes: Dedup rebuild ----------------------------------------------

void RoundTripUnitTypes ()
{
    UnitTypes Original;
    const NominalId Base = NominalId{ 3 };
    SemaType SimpleType;
    SimpleType.Base          = Base;
    const SemaTypeId Simple = Original.Intern( SimpleType );
    Original.SetExprType( Frontend::ExprId{ 1 }, Simple );
    Original.MarkDeferred( Frontend::ExprId{ 2 } );
    Original.SetSiteType( BindingSite{ Frontend::StmtId{ 9 } }, Simple );

    Meta::Writer W;
    Original.SerializeCache( W );
    Meta::Reader R{ W.Data() };
    UnitTypes Restored;
    Check( Restored.DeserializeCache( R ), "UnitTypes deserialize reports success" );

    Check( Restored.ExprType( Frontend::ExprId{ 1 } ) == Simple, "ExprType round-trips at the original SemaTypeId" );
    Check( Restored.IsDeferred( Frontend::ExprId{ 2 } ), "Deferred bit round-trips" );
    Check( not Restored.IsDeferred( Frontend::ExprId{ 1 } ), "a non-deferred expression stays non-deferred" );
    Check( Restored.SiteType( BindingSite{ Frontend::StmtId{ 9 } } ) == Simple, "SiteTypes round-trips" );

    // The whole point of rebuilding Dedup: re-interning the same key must
    // return the *original* Id, not mint a duplicate.
    const SemaTypeId Reinterned = Restored.Intern( SimpleType );
    Check( Reinterned == Simple, "Dedup rebuild makes re-Intern() return the original Id" );
    Check( Restored.Size() == Original.Size(), "no duplicate entry was minted" );
}

// --- InterfaceRegistry -----------------------------------------------------

void RoundTripInterfaceRegistry ()
{
    InterfaceRegistry Original;
    ExportedDecl AppConfig;
    AppConfig.QualifiedName = "Core::AppConfig";
    AppConfig.Kind          = Frontend::DeclKind::Class;
    AppConfig.Unit          = 0;
    AppConfig.Decl          = Frontend::DeclId{ 1 };
    Original.Publish( AppConfig );

    ExportedDecl Square;
    Square.QualifiedName = "MathUtils::square";
    Square.Kind          = Frontend::DeclKind::Method;
    Square.Unit          = 0;
    Square.Decl          = Frontend::DeclId{ 2 };
    Original.Publish( Square );

    Meta::Writer W;
    Original.SerializeCache( W );
    Meta::Reader R{ W.Data() };
    InterfaceRegistry Restored;
    Check( Restored.DeserializeCache( R ), "InterfaceRegistry deserialize reports success" );

    Check( Restored.Size() == Original.Size(), "InterfaceRegistry.Size round-trips" );
    const ExportedDecl *Found = Restored.Lookup( "Core::AppConfig" );
    Check( Found != nullptr and Found->Kind == Frontend::DeclKind::Class, "Lookup resolves a published name after replay" );
}

} // namespace

int main ()
{
    RoundTripTypeStore();
    RoundTripCalleeMapFixup();
    RoundTripScopeTable();
    RoundTripUnitTypes();
    RoundTripInterfaceRegistry();

    if ( Failures == 0 )
    {
        std::println( "SemaSerializeTest: all checks passed" );
        return 0;
    }
    std::println( stderr, "SemaSerializeTest: {} check(s) failed", Failures );
    return 1;
}
