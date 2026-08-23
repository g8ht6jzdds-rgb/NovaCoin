#pragma once

#include "consensus/network_params.hpp"

#include <cstdint>

namespace nova::node
{

enum class NetworkSelectionError : std::uint8_t { kNone, kDisabled, kInvalidParameters };

struct NetworkSelection final {
    NetworkSelectionError error{NetworkSelectionError::kInvalidParameters};
    const consensus::NetworkParams* parameters{};
};

// A compiled table is not activation authority: parameters must validate and
// be marked enabled before a daemon can create a node for that network.
[[nodiscard]] NetworkSelection SelectNetwork(consensus::NetworkId network) noexcept;
[[nodiscard]] const char* NetworkName(consensus::NetworkId network) noexcept;

} // namespace nova::node
