#include "Volt/Core/Log/Logger.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include <unistd.h>

/**
 * Private helpers
 */

namespace
{

using Volt::Core::ELogLevel;

/// One queued record, fully owned so producers never outlive their text.
struct FLogRecord
{

    ELogLevel Level = ELogLevel::Info;
    std::string Step;
    std::string Text;
    bool bFinished = false;
};

/// ANSI fragments; every colored piece is closed by Reset.
namespace Ansi
{

    constexpr std::string_view Reset      = "\x1b[0m";
    constexpr std::string_view Cyan       = "\x1b[36m";
    constexpr std::string_view GrayBold   = "\x1b[1;90m";
    constexpr std::string_view Gray       = "\x1b[90m";
    constexpr std::string_view YellowBold = "\x1b[1;33m";
    constexpr std::string_view RedBold    = "\x1b[1;31m";
    constexpr std::string_view GreenBold  = "\x1b[1;32m";
    constexpr std::string_view EraseLine  = "\r\x1b[2K";

} // namespace Ansi

/// Shared queue state; the worker thread is the only consumer.
struct FLogState
{

    std::mutex Mutex;
    std::condition_variable Pushed; //<< producers -> worker
    std::condition_variable Popped; //<< worker -> Flush() and bounded producers
    std::deque<FLogRecord> Queue;
    std::thread Worker;
    bool bRunning = false;
    bool bClosing = false;
    bool bBusy    = false; //<< worker is rendering a popped record

    std::atomic<ELogLevel> MinLevel{ ELogLevel::Debug };
    std::atomic<bool> bColor{ false };
    std::atomic<bool> bTty{ false }; //<< sticky \r-rewritten progress needs a terminal

    // Safety net for a missing Stop(): a joinable std::thread aborts the
    // process on destruction, so drain and join here instead.
    ~FLogState ()
    {
        {
            const std::scoped_lock Lock( Mutex );
            bClosing = true;
        }
        Pushed.notify_all();
        Popped.notify_all();
        if ( Worker.joinable() )
        {
            Worker.join();
        }
    }
};

constexpr std::size_t QueueCapacity = 1024;

FLogState &State ()
{
    static FLogState Instance;
    return Instance;
}

[[nodiscard]] std::string Paint ( std::string_view Code, std::string_view Text )
{
    if ( !State().bColor.load( std::memory_order_relaxed ) )
    {
        return std::string( Text );
    }
    std::string Out;
    Out.reserve( Code.size() + Text.size() + Ansi::Reset.size() );
    Out += Code;
    Out += Text;
    Out += Ansi::Reset;
    return Out;
}

[[nodiscard]] std::string Symbol ( ELogLevel Level )
{
    switch ( Level )
    {
    case ELogLevel::Debug:
        return Paint( Ansi::Gray, "⚙" );
    case ELogLevel::Info:
        return Paint( Ansi::Cyan, "•" );
    case ELogLevel::Warn:
        return Paint( Ansi::YellowBold, "⚠" );
    case ELogLevel::Error:
        return Paint( Ansi::RedBold, "✗" );
    case ELogLevel::Progress:
        return Paint( Ansi::GreenBold, "➜" );
    }
    return {};
}

[[nodiscard]] std::string StepPart ( const std::string &Step )
{
    if ( Step.empty() )
    {
        return {};
    }
    return Paint( Ansi::GrayBold, " [" + Step + "]" );
}

[[nodiscard]] std::string FormatRecord ( const FLogRecord &Record )
{
    return Symbol( Record.Level ) + StepPart( Record.Step ) + " " + Record.Text;
}

void EraseProgressLine ()
{
    std::cout << Ansi::EraseLine << std::flush;
}

void DrawProgress ( const FLogRecord &Record )
{
    std::cout << '\r' << FormatRecord( Record ) << std::flush;
}

/// Render one record. Only the worker calls this; ActiveProgress is its
/// private redraw state for the sticky progress line.
void Render ( const FLogRecord &Record, std::optional<FLogRecord> &ActiveProgress )
{
    if ( ActiveProgress.has_value() )
    {
        EraseProgressLine();
    }

    if ( Record.Level == ELogLevel::Progress )
    {
        // Without a terminal there is nothing to rewrite in place: each
        // progress update becomes a plain line (and is never re-drawn).
        if ( !State().bTty.load( std::memory_order_relaxed ) )
        {
            std::cout << FormatRecord( Record ) << '\n' << std::flush;
            return;
        }

        DrawProgress( Record );
        if ( Record.bFinished )
        {
            std::cout << '\n' << std::flush;
            ActiveProgress.reset();
        }
        else
        {
            ActiveProgress = Record;
        }
        return;
    }

    std::ostream &Out = ( Record.Level == ELogLevel::Error ) ? std::cerr : std::cout;
    Out << FormatRecord( Record ) << '\n' << std::flush;

    if ( ActiveProgress.has_value() )
    {
        DrawProgress( *ActiveProgress );
    }
}

/// Worker loop: pop in order, render unlocked, signal Flush() waiters.
void WorkerMain ()
{
    std::optional<FLogRecord> ActiveProgress;

    for ( ;; )
    {
        FLogState &S = State();
        std::unique_lock Lock( S.Mutex );
        S.Pushed.wait( Lock, [&S] { return !S.Queue.empty() || S.bClosing; } );

        if ( S.Queue.empty() )
        {
            break; // bClosing and fully drained
        }

        const FLogRecord Record = std::move( S.Queue.front() );
        S.Queue.pop_front();
        S.bBusy = true;
        Lock.unlock();

        Render( Record, ActiveProgress );

        Lock.lock();
        S.bBusy = false;
        S.Popped.notify_all();
    }

    if ( ActiveProgress.has_value() )
    {
        EraseProgressLine();
    }
}

void Enqueue ( FLogRecord Record )
{
    if ( Record.Level != ELogLevel::Progress && Record.Level < State().MinLevel.load( std::memory_order_relaxed ) )
    {
        return;
    }

    Volt::Core::FLogger::Start();

    FLogState &S = State();
    {
        std::unique_lock Lock( S.Mutex );
        S.Popped.wait( Lock, [&S] { return S.Queue.size() < QueueCapacity || S.bClosing; } );
        if ( S.bClosing )
        {
            return;
        }
        S.Queue.push_back( std::move( Record ) );
    }
    S.Pushed.notify_one();
}

} // namespace

/**
 * Public
 */

void Volt::Core::FLogger::Start ()
{
    FLogState &S = State();
    const std::scoped_lock Lock( S.Mutex );
    if ( S.bRunning )
    {
        return;
    }
    S.bRunning = true;
    S.bClosing = false;
    S.bColor.store( StdOutIsTerminal(), std::memory_order_relaxed );
    S.bTty.store( StdOutIsTerminal(), std::memory_order_relaxed );
    S.Worker = std::thread( WorkerMain );
}

void Volt::Core::FLogger::Stop ()
{
    FLogState &S = State();
    {
        const std::scoped_lock Lock( S.Mutex );
        if ( !S.bRunning )
        {
            return;
        }
        S.bClosing = true;
    }
    S.Pushed.notify_all();
    S.Popped.notify_all();
    S.Worker.join();

    const std::scoped_lock Lock( S.Mutex );
    S.Worker   = {};
    S.bRunning = false;
    S.bClosing = false;
}

void Volt::Core::FLogger::Flush ()
{
    FLogState &S = State();
    std::unique_lock Lock( S.Mutex );
    if ( !S.bRunning )
    {
        return;
    }
    S.Popped.wait( Lock, [&S] { return ( S.Queue.empty() && !S.bBusy ) || S.bClosing; } );
}

void Volt::Core::FLogger::SetMinLevel ( ELogLevel Level ) noexcept
{
    State().MinLevel.store( Level, std::memory_order_relaxed );
}

void Volt::Core::FLogger::SetColorEnabled ( bool bEnabled ) noexcept
{
    State().bColor.store( bEnabled, std::memory_order_relaxed );
}

bool Volt::Core::FLogger::StdOutIsTerminal () noexcept
{
    return ::isatty( STDOUT_FILENO ) == 1;
}

void Volt::Core::FLogger::Debug ( std::string Text, std::string Step )
{
    Enqueue( FLogRecord{ .Level = ELogLevel::Debug, .Step = std::move( Step ), .Text = std::move( Text ), .bFinished = false } );
}

void Volt::Core::FLogger::Info ( std::string Text, std::string Step )
{
    Enqueue( FLogRecord{ .Level = ELogLevel::Info, .Step = std::move( Step ), .Text = std::move( Text ), .bFinished = false } );
}

void Volt::Core::FLogger::Warn ( std::string Text, std::string Step )
{
    Enqueue( FLogRecord{ .Level = ELogLevel::Warn, .Step = std::move( Step ), .Text = std::move( Text ), .bFinished = false } );
}

void Volt::Core::FLogger::Error ( std::string Text, std::string Step )
{
    Enqueue( FLogRecord{ .Level = ELogLevel::Error, .Step = std::move( Step ), .Text = std::move( Text ), .bFinished = false } );
}

void Volt::Core::FLogger::Progress ( std::string Text, std::string Step, bool bFinished )
{
    Enqueue( FLogRecord{
        .Level = ELogLevel::Progress, .Step = std::move( Step ), .Text = std::move( Text ), .bFinished = bFinished } );
}
