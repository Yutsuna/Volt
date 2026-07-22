#pragma once

#include "Core_export.hpp"
#include "Volt/Core/Container/NonCopyable.hpp"
#include "Volt/Core/Container/NonMovable.hpp"

#include <cstdint>
#include <string>

namespace Volt
{

namespace Core
{

    /**
     * @enum ELogLevel
     * @brief Severity of a log record; also orders the minimum-level filter.
     */
    enum class ELogLevel : std::uint8_t
    {

        Debug,
        Info,
        Warn,
        Error,
        Progress, //<< transient status line, redrawn in place (never filtered)
    };

    /**
     * @class FLogger
     * @brief Asynchronous CLI logger: producers enqueue, one worker thread renders.
     * @details Any thread may log; records go through a bounded MPSC queue and a
     *          single worker renders them in order (errors to stderr, the rest to
     *          stdout). A Progress record draws a sticky `\r`-rewritten status
     *          line that survives interleaved normal records until it is finished.
     *          Colors are ANSI, enabled automatically when the stream is a TTY.
     */
    class CORE_EXPORT FLogger
    {

    public:

        /** @brief Start the worker thread. Idempotent; logging auto-starts it. */
        static void Start ();

        /** @brief Drain the queue, erase any pending progress line, join the worker. */
        static void Stop ();

        /** @brief Block until every queued record has been rendered. */
        static void Flush ();

    public:

        /** @brief Drop records below this level (Progress is never dropped). */
        static void SetMinLevel ( ELogLevel Level ) noexcept;

        /** @brief Force colors on/off (overrides the TTY autodetection). */
        static void SetColorEnabled ( bool bEnabled ) noexcept;

        /** @brief True when standard output is an interactive terminal. */
        [[nodiscard]] static bool StdOutIsTerminal () noexcept;

    public:

        /** @brief Log a dark-gray `⚙` record (diagnostic chatter). */
        static void Debug ( std::string Text, std::string Step = {} );

        /** @brief Log a cyan `•` record. */
        static void Info ( std::string Text, std::string Step = {} );

        /** @brief Log a yellow `⚠` record. */
        static void Warn ( std::string Text, std::string Step = {} );

        /** @brief Log a red `✗` record on stderr. */
        static void Error ( std::string Text, std::string Step = {} );

        /** @brief Draw/update the sticky `➜` status line; bFinished commits it. */
        static void Progress ( std::string Text, std::string Step = {}, bool bFinished = false );
    };

    /**
     * @class FLogScope
     * @brief RAII owner of the logger worker: Start on construction, Stop on destruction.
     */
    class FLogScope : public FNonCopyable, public FNonMovable
    {

    public:

        FLogScope ()
        {
            FLogger::Start();
        }

        ~FLogScope ()
        {
            FLogger::Stop();
        }
    };

} // namespace Core

} // namespace Volt
