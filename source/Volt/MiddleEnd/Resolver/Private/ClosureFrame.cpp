#include "Volt/MiddleEnd/Resolver/ClosureFrame.hpp"

namespace Volt
{

namespace MiddleEnd::Resolver
{

    ClosureEnvFrame SynthesizeClosureFrame ( const ScopeTable &Scopes, const UnitTypes &Types, ScopeId ClosureScope )
    {
        ClosureEnvFrame Frame;
        Frame.Scope    = ClosureScope;
        Frame.bEscapes = Scopes.Escapes( ClosureScope );

        const auto *Captures = Scopes.CapturesOf( ClosureScope );
        if ( Captures == nullptr or Captures->IsEmpty() )
        {
            return Frame;
        }

        for ( const auto &Cap : *Captures )
        {
            ClosureEnvField Field;
            Field.Name = Cap.Name;
            Field.Site = Cap.Site;
            Field.Type = Types.SiteType( Cap.Site );

            Frame.Fields.PushBack( Field );
        }

        return Frame;
    }

} // namespace MiddleEnd::Resolver

} // namespace Volt
