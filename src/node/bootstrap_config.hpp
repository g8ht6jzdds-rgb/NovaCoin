#pragma once

#include "consensus/network_params.hpp"
#include "net/transport.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nova::node
{

// Signed review of a bootstrap file is an operational release procedure. This
// parser deliberately validates only a bounded, canonical static format; it
// never treats a downloaded manifest or DNS response as authority.
enum class BootstrapConfigError : std::uint8_t {
    kNone,
    kInvalidPath,
    kReadFailure,
    kOversized,
    kNonCanonical,
    kNetworkMismatch,
    kInvalidEndpoint,
    kTooManySeeds,
    kTooManyDnsSeeds,
    kAllocationFailure,
};

struct BootstrapConfigResult final {
    BootstrapConfigError error{BootstrapConfigError::kInvalidPath};
    std::vector<net::TcpEndpoint> seeds;
    // DNS seed names are operational bootstrap hints only.  They neither
    // authorize a peer nor affect consensus; P2P network identity validation
    // remains mandatory after a connection is made.
    std::vector<std::string> dns_seeds;
};

// Returns the reviewed compiled bootstrap registry for a network. The TESTNET
// registry is intentionally empty until signed, candidate-specific operator
// records and the separate TESTNET enablement review have been committed.
// Empty is valid and forces a new node to use --bootstrap or --connect.
[[nodiscard]] BootstrapConfigResult
LoadCompiledBootstrapConfig(const consensus::NetworkParams& network) noexcept;

[[nodiscard]] BootstrapConfigResult
LoadStaticBootstrapConfig(const std::filesystem::path& path,
                          const consensus::NetworkParams& network) noexcept;

} // namespace nova::node
