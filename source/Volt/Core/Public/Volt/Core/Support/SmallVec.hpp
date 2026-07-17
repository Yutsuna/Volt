#pragma once

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

        SmallVec ( std::initializer_list<T> Init )
        {
            Reserve( Init.size() );
            for ( const T &Value : Init )
            {
                PushBack( Value );
            }
        }

        SmallVec ( const SmallVec &Other )
        {
            Reserve( Other.Count );
            for ( SizeType Index = 0; Index < Other.Count; ++Index )
            {
                PushBack( Other.Data[Index] );
            }
        }

        SmallVec ( SmallVec &&Other ) noexcept
        {
            MoveFrom( std::move( Other ) );
        }

        SmallVec &operator=( const SmallVec &Other )
        {
            if ( this != &Other )
            {
                Clear();
                Reserve( Other.Count );
                for ( SizeType Index = 0; Index < Other.Count; ++Index )
                {
                    PushBack( Other.Data[Index] );
                }
            }
            return *this;
        }

        SmallVec &operator=( SmallVec &&Other ) noexcept
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
                Grow( Capacity == 0 ? N : Capacity * 2 );
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

        [[nodiscard]] T *begin ()
        {
            return Data;
        }

        [[nodiscard]] T *end ()
        {
            return Data + Count;
        }

        [[nodiscard]] const T *begin () const
        {
            return Data;
        }

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
            if ( NewCapacity < N )
            {
                NewCapacity = N;
            }

            T *NewData = static_cast<T *>( ::operator new[]( NewCapacity * sizeof( T ), std::align_val_t{ alignof( T ) } ) );

            for ( SizeType Index = 0; Index < Count; ++Index )
            {
                std::construct_at( NewData + Index, std::move( Data[Index] ) );
                std::destroy_at( Data + Index );
            }

            ReleaseStorage();
            Data     = NewData;
            Capacity = NewCapacity;
            bHeap    = true;
        }

        void MoveFrom ( SmallVec &&Other ) noexcept
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
                    std::construct_at( Data + Count, std::move( Other.Data[Index] ) );
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
