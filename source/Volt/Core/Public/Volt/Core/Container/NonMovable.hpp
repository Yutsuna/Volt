#pragma once

namespace Volt
{

/**
 * @class FNonCopyable
 * @brief A class that prevents moving of derived classes.
 */
class FNonMovable
{
public:

    FNonMovable ( FNonMovable && )           = delete;
    FNonMovable &operator=( FNonMovable && ) = delete;

protected:

    FNonMovable ()  = default;
    ~FNonMovable () = default;
};

} // namespace Volt
