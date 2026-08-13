// MonoLookup.cpp — which member a MonoRequest names, and where it is declared.
//
// A MonoRequest names the *receiver*, not the declaring type, so the lookup goes
// through LookupMember's own order — own body first, then mixins, then the
// superclass. That is exactly what makes an inherited default (`Arithmetic#min`
// called on `Int32`) drainable: the request's Owner never declares the member
// itself.

#include "Lower/Mono/MonoDriver.hpp"

const Volt::MiddleEnd::TypeSystem::Member *Volt::Backend::Llvm::MonoDriver::LookupMonoMember ( const MonoRequest &Request,
                                                                                               const UnitView **OutUnit ) const
{
    const MiddleEnd::TypeSystem::TypeStore &Store = *Services->Build->Types;

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

    for ( const UnitView &View : Services->Build->Units )
    {
        if ( View.Ordinal == Found->Unit )
        {
            *OutUnit = &View;
            return Found;
        }
    }
    return nullptr;
}
