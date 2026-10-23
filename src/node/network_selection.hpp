#pragma once

#include "consensus/network_params.hpp"

#include <cstdint>

namespace nova::node
{

// Node-local defense in depth, not a consensus parameter or runtime switch.
// Deliberately no CLI/environment/CMake override. TASK022 never unlocks MAINNET.
inline constexpr bool MAINNET_LOCKED = true;
inline constexpr const char* MAINNET_LOCK_REASON =
    "MAINNET_LOCKED: mainnet startup is forbidden. Mandatory readiness gates, "
    "external human security review and explicit activation approval are incomplete.";

enum class NetworkSelectionError : std::uint8_t {
    kNone,
    kDisabled,
    kInvalidParameters,
    kMainnetLocked
};

struct NetworkSelection final {
    NetworkSelectionError error{NetworkSelectionError::kInvalidParameters};
    const consensus::NetworkParams* parameters{};
};

// A compiled table is not activation authority: parameters must validate and
// be marked enabled before a daemon can create a node for that network.
[[nodiscard]] NetworkSelection SelectNetwork(consensus::NetworkId network) noexcept;
[[nodiscard]] const char* NetworkName(consensus::NetworkId network) noexcept;

} // namespace nova::node
