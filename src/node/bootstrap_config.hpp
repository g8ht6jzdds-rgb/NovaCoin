#pragma once

#include "consensus/network_params.hpp"
#include "net/transport.hpp"

#include <cstdint>
#include <filesystem>
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
    kAllocationFailure,
};

struct BootstrapConfigResult final {
    BootstrapConfigError error{BootstrapConfigError::kInvalidPath};
    std::vector<net::TcpEndpoint> seeds;
};

[[nodiscard]] BootstrapConfigResult
LoadStaticBootstrapConfig(const std::filesystem::path& path,
                          const consensus::NetworkParams& network) noexcept;

} // namespace nova::node
