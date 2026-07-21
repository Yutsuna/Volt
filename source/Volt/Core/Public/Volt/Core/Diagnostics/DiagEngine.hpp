#pragma once

#include "Volt/Core/Container/NonCopyable.hpp"
#include "Volt/Core/Diagnostics/Diagnostic.hpp"

#include <cstddef>
#include <mutex>
#include <ostream>
#include <string>
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

            /**
             * @brief Report a diagnostic into the bag.
             * @param Diag The diagnostic to report.
             * @return A reference to the reported diagnostic.
             */
            Diagnostic &Report ( Diagnostic Diag );

            /**
             * @brief Report an error diagnostic into the bag.
             * @param Range The source range associated with the diagnostic.
             * @param Message The error message.
             * @return A reference to the reported diagnostic.
             */
            Diagnostic &Error ( SourceRange Range, std::string Message );

            /**
             * @brief Report a warning diagnostic into the bag.
             * @param Range The source range associated with the diagnostic.
             * @param Message The warning message.
             * @return A reference to the reported diagnostic.
             */
            Diagnostic &Warning ( SourceRange Range, std::string Message );

            /**
             * @brief Get the number of error diagnostics in the bag.
             * @return The number of error diagnostics.
             */
            [[nodiscard]] std::size_t Errors () const noexcept;

        private:

            friend class DiagEngine;

            std::vector<Diagnostic> Items;
            std::size_t ErrorCount = 0;
        };

        /**
         * @brief Create a new empty Bag for thread-local accumulation of diagnostics.
         * @return A new empty Bag.
         */
        [[nodiscard]] static Bag MakeBag () noexcept;

        /**
         * @brief Fold a thread-local Bag into the shared store.
         * @param Local The thread-local Bag to merge.
         */
        void Merge ( Bag &&Local );

        /**
         * @brief Report a single diagnostic directly into the shared store.
         * @param Diag The diagnostic to report.
         * @return A reference to the reported diagnostic.
         */
        Diagnostic &Report ( Diagnostic Diag );

        /**
         * @brief Get the total number of error diagnostics in the shared store.
         * @return The total number of error diagnostics.
         */
        [[nodiscard]] std::size_t ErrorTotal () const noexcept;

        /**
         * @brief Check if there are any error diagnostics in the shared store.
         * @return True if there are error diagnostics, false otherwise.
         */
        [[nodiscard]] bool HasErrors () const noexcept;

        /**
         * @brief Get the total number of diagnostics in the shared store.
         * @return The total number of diagnostics.
         */
        [[nodiscard]] std::size_t Count () const noexcept;

        /**
         * @brief Pretty-print Render all diagnostics in the shared store to the given output stream.
         * @param Sources The source manager for resolving source locations.
         * @param Out The output stream to render diagnostics to.
         */
        void Render ( const SourceManager &Sources, std::ostream &Out ) const;

    private:

        mutable std::mutex Mutex;
        std::vector<Diagnostic> Store;
        std::size_t ErrorCount = 0;
    };

} // namespace Core

} // namespace Volt
