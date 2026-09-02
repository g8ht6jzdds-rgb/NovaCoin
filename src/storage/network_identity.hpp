#pragma once

#include "consensus/network_params.hpp"

#include <cstdint>
#include <filesystem>

namespace nova::storage
{

// This is a local storage admission guard, not a consensus serialization.
// It prevents an operator from accidentally replaying one network's journal
// and wallet directory under another network selection.
enum class NetworkIdentityError : std::uint8_t {
    kNone,
    kInvalidParameters,
    kFilesystemFailure,
    kLegacyDirectoryWithoutMarker,
    kMalformedMarker,
    kNetworkMismatch,
    kWriteFailure,
};

[[nodiscard]] NetworkIdentityError
EnsureNetworkIdentity(const std::filesystem::path& data_directory,
                      const consensus::NetworkParams& network) noexcept;

[[nodiscard]] std::filesystem::path
NetworkIdentityMarkerPath(const std::filesystem::path& data_directory) noexcept;

} // namespace nova::storage
