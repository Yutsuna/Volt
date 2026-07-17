#pragma once

#include "Volt/Core/Container/NonCopyable.hpp"
#include "Volt/Core/Diagnostics/Diagnostic.hpp"

#include <cstddef>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace Volt
{

namespace Core
{

    class SourceManager;

    /**
     * @class DiagEngine
     * @brief A thread-safe diagnostic engine for accumulating and reporting diagnostics.
     * @details Parse/sema threads accumulate into a local Bag,
     *          then Merge it under one lock at the end of a pass.
     *          Direct Report() is also available for single-threaded use.
     */
    class DiagEngine
    {

    public:

        /**
         * @class Bag
         * @brief Lock-free per-thread accumulator for diagnostics.
         */
        class Bag : public FNonCopyable
        {

        public:

            Bag ()  = default;
            ~Bag () = default;

        public:

            Diagnostic &Report ( Diagnostic Diag )
            {
                if ( Diag.Severity == ESeverity::Error )
                {
                    ++ErrorCount;
                }
                return Items.emplace_back( std::move( Diag ) );
            }

            Diagnostic &Error ( SourceRange Range, std::string Message )
            {
                return Report( Diagnostic{ ESeverity::Error, Range, std::move( Message ), {} } );
            }

            Diagnostic &Warning ( SourceRange Range, std::string Message )
            {
                return Report( Diagnostic{ ESeverity::Warning, Range, std::move( Message ), {} } );
            }

            [[nodiscard]] std::size_t Errors () const
            {
                return ErrorCount;
            }

        private:

            friend class DiagEngine;

            std::vector<Diagnostic> Items;
            std::size_t ErrorCount = 0;
        };

        [[nodiscard]] Bag MakeBag () const
        {
            return Bag{};
        }

        /// Fold a thread-local Bag into the shared store (takes the lock).
        void Merge ( Bag &&Local )
        {
            const std::scoped_lock Guard{ Mutex };
            for ( Diagnostic &Diag : Local.Items )
            {
                Store.push_back( std::move( Diag ) );
            }
            ErrorCount += Local.ErrorCount;
            Local.Items.clear();
            Local.ErrorCount = 0;
        }

        /// Report a single diagnostic directly (takes the lock).
        Diagnostic &Report ( Diagnostic Diag )
        {
            const std::scoped_lock Guard{ Mutex };
            if ( Diag.Severity == ESeverity::Error )
            {
                ++ErrorCount;
            }
            return Store.emplace_back( std::move( Diag ) );
        }

        [[nodiscard]] std::size_t ErrorTotal () const
        {
            const std::scoped_lock Guard{ Mutex };
            return ErrorCount;
        }

        [[nodiscard]] bool HasErrors () const
        {
            return ErrorTotal() != 0;
        }

        [[nodiscard]] std::size_t Count () const
        {
            const std::scoped_lock Guard{ Mutex };
            return Store.size();
        }

        /// Pretty-print every buffered diagnostic to Out.
        void Render ( const SourceManager &Sources, std::ostream &Out ) const;

    private:

        mutable std::mutex Mutex;
        std::vector<Diagnostic> Store;
        std::size_t ErrorCount = 0;
    };

} // namespace Core

} // namespace Volt
