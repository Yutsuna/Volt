// InstantiationCache.cpp — the memo behind ReinstantiateBody.

#include "Volt/MiddleEnd/Analysis/InstantiationCache.hpp"

#include <mutex>
#include <utility>

namespace Volt
{

namespace MiddleEnd
{

    namespace Analysis
    {

        InstantiationCache &InstantiationCache::Global ()
        {
            static InstantiationCache Instance;
            return Instance;
        }

        std::optional<TypeSystem::InstantiatedBody> InstantiationCache::Lookup ( const MonoKey &Key )
        {
            const std::lock_guard<std::mutex> Guard( Lock );

            const auto It = Entries.find( Key );
            if ( It == Entries.end() )
            {
                MissCount.fetch_add( 1, std::memory_order_relaxed );
                return std::nullopt;
            }

            It->second.Tick = ++Clock;
            HitCount.fetch_add( 1, std::memory_order_relaxed );
            return It->second.Body;
        }

        void InstantiationCache::Insert ( MonoKey Key, const TypeSystem::InstantiatedBody &Body )
        {
            const std::lock_guard<std::mutex> Guard( Lock );

            if ( Budget == 0 )
            {
                return;
            }
            EvictDownTo( Budget - 1 );
            Entries.insert_or_assign( std::move( Key ), Entry{ .Body = Body, .Tick = ++Clock } );
        }

        bool InstantiationCache::Enter ( const MonoKey &Key )
        {
            const std::lock_guard<std::mutex> Guard( Lock );

            if ( not InFlight.insert( Key ).second )
            {
                CycleCount.fetch_add( 1, std::memory_order_relaxed );
                return false;
            }
            return true;
        }

        void InstantiationCache::Leave ( const MonoKey &Key )
        {
            const std::lock_guard<std::mutex> Guard( Lock );
            InFlight.erase( Key );
        }

        void InstantiationCache::SetBudget ( const std::size_t MaxEntries )
        {
            const std::lock_guard<std::mutex> Guard( Lock );
            Budget = MaxEntries;
            EvictDownTo( Budget );
        }

        void InstantiationCache::Clear ()
        {
            const std::lock_guard<std::mutex> Guard( Lock );
            Entries.clear();
            InFlight.clear();
            Clock = 0;
        }

        std::size_t InstantiationCache::Size () const
        {
            const std::lock_guard<std::mutex> Guard( Lock );
            return Entries.size();
        }

        void InstantiationCache::EvictDownTo ( const std::size_t MaxEntries )
        {
            // A linear scan per eviction, deliberately: the budget is small, an
            // eviction is rare next to the instantiation it makes room for, and
            // an intrusive LRU list would be more machinery than the saving is
            // worth at this size.
            while ( Entries.size() > MaxEntries )
            {
                auto Oldest = Entries.begin();
                for ( auto It = Entries.begin(); It != Entries.end(); ++It )
                {
                    if ( It->second.Tick < Oldest->second.Tick )
                    {
                        Oldest = It;
                    }
                }
                Entries.erase( Oldest );
                EvictCount.fetch_add( 1, std::memory_order_relaxed );
            }
        }

    } // namespace Analysis

} // namespace MiddleEnd

} // namespace Volt
