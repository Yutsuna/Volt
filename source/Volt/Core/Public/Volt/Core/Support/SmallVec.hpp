#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace Volt
{

namespace Core
{

    /// Vector with an inline buffer of N elements: no heap allocation until
    /// the count exceeds N. Most AST nodes hold a handful of child Ids, so
    /// this keeps them flat and cache-local without per-node allocation.
    template <typename T, std::size_t N> class SmallVec
    {

    public:

        using ValueType = T;
        using SizeType  = std::size_t;

        SmallVec () = default;

        // Delegating to the default constructor first means *this is a live
        // object during the copy loop: if an element copy throws, ~SmallVec
        // runs and releases whatever was already constructed.
        SmallVec ( std::initializer_list<T> Init ) : SmallVec()
        {
            Reserve( Init.size() );
            for ( const T &Value : Init )
            {
                PushBack( Value );
            }
        }

        SmallVec ( const SmallVec &Other ) : SmallVec()
        {
            Reserve( Other.Count );
            for ( SizeType Index = 0; Index < Other.Count; ++Index )
            {
                PushBack( Other.Data[Index] );
            }
        }

        SmallVec ( SmallVec &&Other ) noexcept( std::is_nothrow_move_constructible_v<T> )
        {
            MoveFrom( std::move( Other ) );
        }

        // Copy-and-swap shape: build the copy aside, then steal it, so *this
        // keeps its old contents if any element copy throws (strong guarantee
        // whenever T's move cannot throw).
        SmallVec &operator=( const SmallVec &Other )
        {
            if ( this != &Other )
            {
                SmallVec Temp{ Other };
                *this = std::move( Temp );
            }
            return *this;
        }

        SmallVec &operator=( SmallVec &&Other ) noexcept( std::is_nothrow_move_constructible_v<T> )
        {
            if ( this != &Other )
            {
                Destroy();
                MoveFrom( std::move( Other ) );
            }
            return *this;
        }

        ~SmallVec ()
        {
            Destroy();
        }

        void PushBack ( const T &Value )
        {
            EmplaceBack( Value );
        }

        void PushBack ( T &&Value )
        {
            EmplaceBack( std::move( Value ) );
        }

        template <typename... Args> T &EmplaceBack ( Args &&...InArgs )
        {
            if ( Count == Capacity )
            {
                // InArgs may alias our own storage (Vec.PushBack( Vec[0] )):
                // materialise the value before Grow relocates the elements.
                T Value( std::forward<Args>( InArgs )... );
                Grow( Capacity == 0 ? N : Capacity * 2 );
                T *Slot = std::construct_at( Data + Count, std::move( Value ) );
                ++Count;
                return *Slot;
            }
            T *Slot = std::construct_at( Data + Count, std::forward<Args>( InArgs )... );
            ++Count;
            return *Slot;
        }

        [[nodiscard]] T &operator[]( SizeType Index )
        {
            return Data[Index];
        }

        [[nodiscard]] const T &operator[]( SizeType Index ) const
        {
            return Data[Index];
        }

        [[nodiscard]] SizeType Size () const
        {
            return Count;
        }

        [[nodiscard]] bool IsEmpty () const
        {
            return Count == 0;
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        [[nodiscard]] T *begin ()
        {
            return Data;
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        [[nodiscard]] T *end ()
        {
            return Data + Count;
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        [[nodiscard]] const T *begin () const
        {
            return Data;
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        [[nodiscard]] const T *end () const
        {
            return Data + Count;
        }

        void Clear ()
        {
            for ( SizeType Index = 0; Index < Count; ++Index )
            {
                std::destroy_at( Data + Index );
            }
            Count = 0;
        }

        void Reserve ( SizeType NewCapacity )
        {
            if ( NewCapacity > Capacity )
            {
                Grow( NewCapacity );
            }
        }

    private:

        [[nodiscard]] T *InlineData ()
        {
            return reinterpret_cast<T *>( Inline );
        }

        void Grow ( SizeType NewCapacity )
        {
            NewCapacity = std::max( NewCapacity, N );

            T *NewData = static_cast<T *>( ::operator new[]( NewCapacity * sizeof( T ), std::align_val_t{ alignof( T ) } ) );

            // move_if_noexcept: fall back to copying when T's move can throw,
            // so a throw mid-relocation leaves the old buffer intact.
            SizeType Relocated = 0;
            try
            {
                for ( ; Relocated < Count; ++Relocated )
                {
                    std::construct_at( NewData + Relocated, std::move_if_noexcept( Data[Relocated] ) );
                }
            }
            catch ( ... )
            {
                std::destroy_n( NewData, Relocated );
                ::operator delete[]( NewData, std::align_val_t{ alignof( T ) } );
                throw;
            }

            std::destroy_n( Data, Count );
            ReleaseStorage();
            Data     = NewData;
            Capacity = NewCapacity;
            bHeap    = true;
        }

        void MoveFrom ( SmallVec &&Other ) noexcept( std::is_nothrow_move_constructible_v<T> )
        {
            if ( Other.bHeap )
            {
                Data     = Other.Data;
                Count    = Other.Count;
                Capacity = Other.Capacity;
                bHeap    = true;

                Other.Data     = Other.InlineData();
                Other.Count    = 0;
                Other.Capacity = N;
                Other.bHeap    = false;
            }
            else
            {
                Data     = InlineData();
                Capacity = N;
                bHeap    = false;
                Count    = 0;
                for ( SizeType Index = 0; Index < Other.Count; ++Index )
                {
                    std::construct_at( Data + Count, std::move_if_noexcept( Other.Data[Index] ) );
                    ++Count;
                }
                Other.Clear();
            }
        }

        void ReleaseStorage ()
        {
            if ( bHeap )
            {
                ::operator delete[]( Data, std::align_val_t{ alignof( T ) } );
            }
        }

        void Destroy ()
        {
            Clear();
            ReleaseStorage();
        }

        alignas( T ) std::byte Inline[N * sizeof( T )] = {};

        T *Data           = InlineData();
        SizeType Count    = 0;
        SizeType Capacity = N;
        bool bHeap        = false;
    };

} // namespace Core

} // namespace Volt
