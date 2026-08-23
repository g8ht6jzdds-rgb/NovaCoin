#pragma once

#include "consensus/network_params.hpp"
#include "crypto/crypto.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace nova::wallet
{

// Presentation-only Base58Check address. The network prefix is mandatory and
// must be checked by every wallet, faucet, and RPC boundary.
[[nodiscard]] std::optional<std::string>
EncodeP2pkhAddress(const crypto::Hash160& key_hash,
                   const consensus::NetworkParams& network) noexcept;
[[nodiscard]] std::optional<crypto::Hash160>
DecodeP2pkhAddress(std::string_view encoded,
                   const consensus::NetworkParams& expected_network) noexcept;

} // namespace nova::wallet
