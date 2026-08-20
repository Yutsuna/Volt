// ProcessRunner.cpp — the one place Volt starts a host process.
//
// Compile-time command literals (`` `git rev-parse HEAD` ``) are the only
// caller, and they run inside a compiler: the contract is therefore "always
// returns, always bounded". Both streams are drained concurrently through
// poll() — reading one to completion before the other deadlocks the moment a
// command writes more than a pipe buffer to the stream nobody is reading yet.

#include "Volt/Core/Support/ProcessRunner.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#if defined( _WIN32 )

Volt::Core::ProcessResult
Volt::Core::RunShell ( std::string_view Command, std::string_view WorkDir, ProcessLimits Limits )
{
    static_cast<void>( Command );
    static_cast<void>( WorkDir );
    static_cast<void>( Limits );

    // Deliberately a reported failure rather than a silent empty string: a
    // build that quietly bakes "" into a constant is worse than one that stops.
    ProcessResult Result;
    Result.bSpawnFailed = true;
    Result.Err          = "compile-time command execution is not implemented on this host platform";
    return Result;
}

#else

    #include <cerrno>
    #include <signal.h>
    #include <poll.h>
    #include <sys/wait.h>
    #include <unistd.h>

namespace
{

using Clock = std::chrono::steady_clock;

// A pipe that closes itself. Every early return below would otherwise leak a
// descriptor per command, and a macro-heavy build runs thousands.
class Pipe
{

public:

    Pipe () : Fds{ -1, -1 }, bOpen( ::pipe( Fds ) == 0 )
    {
    }

    ~Pipe ()
    {
        CloseRead();
        CloseWrite();
    }

    Pipe ( const Pipe & )            = delete;
    Pipe &operator= ( const Pipe & ) = delete;
    Pipe ( Pipe && )                 = delete;
    Pipe &operator= ( Pipe && )      = delete;

    [[nodiscard]] bool Ok () const
    {
        return bOpen;
    }

    [[nodiscard]] int Read () const
    {
        return Fds[0];
    }

    [[nodiscard]] int Write () const
    {
        return Fds[1];
    }

    void CloseRead ()
    {
        if ( Fds[0] >= 0 )
        {
            ::close( Fds[0] );
            Fds[0] = -1;
        }
    }

    void CloseWrite ()
    {
        if ( Fds[1] >= 0 )
        {
            ::close( Fds[1] );
            Fds[1] = -1;
        }
    }

private:

    int Fds[2];
    bool bOpen;
};

// Reap the child, killing it first when the deadline passed. Called on every
// path out, so a timed-out command never survives its own build.
void Reap ( ::pid_t Child, bool bKill, int &Status )
{
    if ( bKill )
    {
        ::kill( Child, SIGKILL );
    }
    while ( ::waitpid( Child, &Status, 0 ) < 0 and errno == EINTR )
    {
    }
}

void AppendCapped ( std::string &Out, const char *Data, std::size_t Count, std::size_t MaxBytes, bool &bTruncated )
{
    if ( Out.size() >= MaxBytes )
    {
        bTruncated = true;
        return;
    }
    const std::size_t Room = MaxBytes - Out.size();
    if ( Count > Room )
    {
        bTruncated = true;
        Count      = Room;
    }
    Out.append( Data, Count );
}

} // namespace

Volt::Core::ProcessResult
Volt::Core::RunShell ( std::string_view Command, std::string_view WorkDir, ProcessLimits Limits )
{
    ProcessResult Result;

    Pipe OutPipe;
    Pipe ErrPipe;
    if ( not OutPipe.Ok() or not ErrPipe.Ok() )
    {
        Result.bSpawnFailed = true;
        Result.Err          = "could not create a pipe for the compile-time command";
        return Result;
    }

    // Copied into owning storage before the fork: the child may only call
    // async-signal-safe functions, so nothing between fork and exec is allowed
    // to allocate — including a string_view's missing terminator.
    const std::string CommandText( Command );
    const std::string WorkDirText( WorkDir );

    const ::pid_t Child = ::fork();
    if ( Child < 0 )
    {
        Result.bSpawnFailed = true;
        Result.Err          = "could not fork for the compile-time command";
        return Result;
    }

    if ( Child == 0 )
    {
        if ( not WorkDirText.empty() and ::chdir( WorkDirText.c_str() ) != 0 )
        {
            ::_exit( 127 );
        }
        if ( ::dup2( OutPipe.Write(), STDOUT_FILENO ) < 0 or ::dup2( ErrPipe.Write(), STDERR_FILENO ) < 0 )
        {
            ::_exit( 127 );
        }
        // execv, not execl: a fixed argv array says the same thing without a
        // vararg call, and the child may not allocate to build one.
        char *const Argv[] = { const_cast<char *>( "sh" ), const_cast<char *>( "-c" ),
                               const_cast<char *>( CommandText.c_str() ), nullptr };
        ::execv( "/bin/sh", Argv );
        ::_exit( 127 ); // exec only returns on failure
    }

    OutPipe.CloseWrite();
    ErrPipe.CloseWrite();

    const auto Deadline = Clock::now() + std::chrono::milliseconds( Limits.TimeoutMs );

    ::pollfd Fds[2]  = { { .fd = OutPipe.Read(), .events = POLLIN, .revents = 0 },
                         { .fd = ErrPipe.Read(), .events = POLLIN, .revents = 0 } };
    std::string *Sinks[2] = { &Result.Out, &Result.Err };
    bool bDone[2]         = { false, false };

    std::vector<char> Buffer( 4096 );

    while ( not bDone[0] or not bDone[1] )
    {
        const auto Remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>( Deadline - Clock::now() ).count();
        if ( Remaining <= 0 )
        {
            Result.bTimedOut = true;
            break;
        }

        const int Ready = ::poll( Fds, 2, static_cast<int>( Remaining ) );
        if ( Ready < 0 )
        {
            if ( errno == EINTR )
            {
                continue;
            }
            Result.bSpawnFailed = true;
            Result.Err          = "poll failed while reading the compile-time command's output";
            break;
        }
        if ( Ready == 0 )
        {
            Result.bTimedOut = true;
            break;
        }

        for ( std::size_t Index = 0; Index < 2; ++Index )
        {
            if ( bDone[Index] or Fds[Index].revents == 0 )
            {
                continue;
            }

            const ::ssize_t Count = ::read( Fds[Index].fd, Buffer.data(), Buffer.size() );
            if ( Count > 0 )
            {
                AppendCapped( *Sinks[Index], Buffer.data(), static_cast<std::size_t>( Count ), Limits.MaxBytes,
                              Result.bTruncated );
                continue;
            }
            if ( Count < 0 and ( errno == EINTR or errno == EAGAIN ) )
            {
                continue;
            }
            // EOF, or an error there is nothing left to do about: this stream
            // is finished either way, and poll must stop waiting on it.
            bDone[Index]      = true;
            Fds[Index].fd     = -1;
            Fds[Index].events = 0;
        }
    }

    int Status = 0;
    Reap( Child, Result.bTimedOut, Status );

    if ( not Result.bTimedOut and not Result.bSpawnFailed )
    {
        Result.ExitCode = WIFEXITED( Status ) ? WEXITSTATUS( Status ) : -1;
    }
    return Result;
}

#endif
