#include "Volt/Sema/Layout/ClosureFrame.hpp"

#include <algorithm>

namespace Volt
{

namespace Sema
{

    ClosureEnvFrame SynthesizeClosureFrame ( const ScopeTable &Scopes, const UnitTypes &Types, ScopeId ClosureScope )
    {
        ClosureEnvFrame Frame;
        Frame.Scope = ClosureScope;

        const auto *Captures = Scopes.CapturesOf( ClosureScope );
        if ( Captures == nullptr or Captures->IsEmpty() )
        {
            Frame.TotalSize = 0;
            Frame.Alignment = 1;
            return Frame;
        }

        std::size_t CurrentOffset = 0;
        std::size_t MaxAlignment  = 1;

        for ( const auto &Cap : *Captures )
        {
            ClosureEnvField Field;
            Field.Name = Cap.Name;
            Field.Site = Cap.Site;
            Field.Type = Types.SiteType( Cap.Site );

            constexpr std::size_t FieldSize  = 8;
            constexpr std::size_t FieldAlign = 8;

            if ( CurrentOffset % FieldAlign != 0 )
            {
                CurrentOffset += ( FieldAlign - ( CurrentOffset % FieldAlign ) );
            }

            Field.Offset = CurrentOffset;
            CurrentOffset += FieldSize;
            MaxAlignment = std::max( MaxAlignment, FieldAlign );

            Frame.Fields.PushBack( Field );
        }

        if ( MaxAlignment > 0 and CurrentOffset % MaxAlignment != 0 )
        {
            CurrentOffset += ( MaxAlignment - ( CurrentOffset % MaxAlignment ) );
        }

        Frame.TotalSize = CurrentOffset;
        Frame.Alignment = MaxAlignment;

        return Frame;
    }

} // namespace Sema

} // namespace Volt
