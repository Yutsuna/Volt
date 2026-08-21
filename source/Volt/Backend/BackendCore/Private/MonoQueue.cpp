// MonoQueue.cpp — the drain loop, and which member a request names.
//
// See Monomorphizer.hpp. Both halves are target-neutral: one is scheduling, the
// other is a TypeStore read. What a target adds is the body emission `Drain`
// calls back into.

#include "Volt/BackendCore/Monomorphizer.hpp"

#include <optional>

void Volt::Backend::MonoQueue::Drain ( Monomorphizer &Queue,
                                       const std::function<bool()> &ShouldStop,
                                       const std::function<void( const MonoRequest & )> &Emit )
{
    while ( not ShouldStop() )
    {
        const std::optional<MonoRequest> Request = Queue.Next();
        if ( not Request.has_value() )
        {
            return;
        }
        Emit( *Request );
    }
}

const Volt::MiddleEnd::TypeSystem::Member *
Volt::Backend::MonoQueue::Lookup ( const BackendInput &Build, const MonoRequest &Request, const UnitView **OutUnit )
{
    if ( Build.Types == nullptr )
    {
        return nullptr;
    }
    const MiddleEnd::TypeSystem::TypeStore &Store = *Build.Types;

    const MiddleEnd::TypeSystem::Member *Found = nullptr;
    if ( Request.Owner.IsValid() )
    {
        Found = Store.LookupMember( Request.Owner, Store.Text( Request.Name ) ).Decl;
    }
    else
    {
        for ( const MiddleEnd::TypeSystem::Member &Candidate : Store.FreeFunctions() )
        {
            if ( Candidate.Name == Request.Name )
            {
                Found = &Candidate;
                break;
            }
        }
    }
    if ( Found == nullptr )
    {
        return nullptr;
    }

    for ( const UnitView &View : Build.Units )
    {
        if ( View.Ordinal == Found->Unit )
        {
            *OutUnit = &View;
            return Found;
        }
    }
    return nullptr;
}
