#pragma once

#include "consensus/network_params.hpp"

#include <cstdint>
#include <filesystem>
#include <span>

namespace nova::storage
{
#ifndef _WIN32
// Persist the directory entries that name an existing node directory and its ancestors.
[[nodiscard]] bool SyncDirectoryHierarchy(const std::filesystem::path& path) noexcept;
#endif

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
ValidateNetworkIdentity(std::span<const std::uint8_t> encoded,
                        const consensus::NetworkParams& network) noexcept;

[[nodiscard]] NetworkIdentityError
EnsureNetworkIdentity(const std::filesystem::path& data_directory,
                      const consensus::NetworkParams& network) noexcept;

// Caller must hold the node data-directory lease. Allows only its reserved
// directory alongside an absent marker; other pre-existing contents still fail.
[[nodiscard]] NetworkIdentityError
EnsureNetworkIdentityWithLease(const std::filesystem::path& data_directory,
                               const consensus::NetworkParams& network) noexcept;

[[nodiscard]] std::filesystem::path
NetworkIdentityMarkerPath(const std::filesystem::path& data_directory) noexcept;

} // namespace nova::storage
