#pragma once

#include <cstddef>

namespace nova::rpc
{

// Current node RPC HTTP framing limits. Keep the server and bounded parser
// fuzz target bound to the same values.
inline constexpr std::size_t kNodeRpcHttpHeaderBytes = 8'192U;
inline constexpr std::size_t kNodeRpcHttpBodyBytes = 1'000'000U;

} // namespace nova::rpc
