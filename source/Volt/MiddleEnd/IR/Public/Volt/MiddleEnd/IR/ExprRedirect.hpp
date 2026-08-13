#pragma once

#include "Volt/Frontend/AST/Node.hpp"

#include <cstdint>
#include <unordered_map>

namespace Volt
{

namespace MiddleEnd
{

namespace IR
{

    using ExprRedirectMap = std::unordered_map<std::uint32_t, Frontend::ExprId>;

} // namespace IR

} // namespace MiddleEnd

} // namespace Volt
