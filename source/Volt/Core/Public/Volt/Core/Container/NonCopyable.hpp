#pragma once

namespace Volt
{

/**
 * @class FNonCopyable
 * @brief A base class that prevents copying and assignment of derived classes.
 */
class FNonCopyable
{

public:

    FNonCopyable ( const FNonCopyable & )           = delete;
    FNonCopyable &operator=( const FNonCopyable & ) = delete;

protected:

    FNonCopyable ()  = default;
    ~FNonCopyable () = default;
};

} // namespace Volt
