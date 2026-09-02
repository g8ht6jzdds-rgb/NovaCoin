#include "storage/network_identity.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nova::storage
{
namespace
{

constexpr std::array<std::uint8_t, 4U> kMarkerMagic{'N', 'V', 'I', 'D'};
constexpr std::uint32_t kMarkerVersion = 1U;
constexpr std::size_t kMarkerSize = 4U + 4U + 1U + 4U + crypto::Hash256::kSize;

void WriteU32(std::array<std::uint8_t, kMarkerSize>& output, const std::size_t offset,
              const std::uint32_t value) noexcept
{
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        output[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

[[nodiscard]] std::uint32_t ReadU32(const std::array<std::uint8_t, kMarkerSize>& input,
                                    const std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(input[offset]) |
           (static_cast<std::uint32_t>(input[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(input[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(input[offset + 3U]) << 24U);
}

[[nodiscard]] bool IsEmptyDirectory(const std::filesystem::path& directory,
                                    std::error_code& error) noexcept
{
    const std::filesystem::directory_iterator end;
    return std::filesystem::directory_iterator{directory, error} == end && !error;
}

[[nodiscard]] bool WriteAtomically(const std::filesystem::path& path,
                                   const std::array<std::uint8_t, kMarkerSize>& bytes) noexcept
{
    const auto temporary = path.string() + ".tmp";
    std::error_code error;
    std::filesystem::remove(temporary, error);
    try {
        {
            std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
            if (!output.is_open()) {
                return false;
            }
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output.good()) {
                output.close();
                std::filesystem::remove(temporary, error);
                return false;
            }
        }
#ifdef _WIN32
        const auto handle = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE || FlushFileBuffers(handle) == 0) {
            if (handle != INVALID_HANDLE_VALUE) {
                static_cast<void>(CloseHandle(handle));
            }
            std::filesystem::remove(temporary, error);
            return false;
        }
        static_cast<void>(CloseHandle(handle));
        if (!MoveFileExA(temporary.c_str(), path.string().c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary, error);
            return false;
        }
#else
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return false;
        }
#endif
        return true;
    } catch (...) {
        std::filesystem::remove(temporary, error);
        return false;
    }
}

} // namespace

std::filesystem::path
NetworkIdentityMarkerPath(const std::filesystem::path& data_directory) noexcept
{
    return data_directory / "network.identity";
}

NetworkIdentityError EnsureNetworkIdentity(const std::filesystem::path& data_directory,
                                           const consensus::NetworkParams& network) noexcept
{
    if (data_directory.empty() ||
        consensus::CheckNetworkParams(network) != consensus::NetworkParamsError::kNone) {
        return NetworkIdentityError::kInvalidParameters;
    }
    try {
        std::error_code error;
        std::filesystem::create_directories(data_directory, error);
        if (error) {
            return NetworkIdentityError::kFilesystemFailure;
        }
        const auto path = NetworkIdentityMarkerPath(data_directory);
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            return NetworkIdentityError::kFilesystemFailure;
        }
        if (!exists) {
            if (!IsEmptyDirectory(data_directory, error)) {
                return error ? NetworkIdentityError::kFilesystemFailure
                             : NetworkIdentityError::kLegacyDirectoryWithoutMarker;
            }
            std::array<std::uint8_t, kMarkerSize> encoded{};
            std::copy(kMarkerMagic.begin(), kMarkerMagic.end(), encoded.begin());
            WriteU32(encoded, 4U, kMarkerVersion);
            encoded[8U] = static_cast<std::uint8_t>(network.id);
            WriteU32(encoded, 9U, network.network_magic);
            std::copy(network.genesis_hash.bytes().begin(), network.genesis_hash.bytes().end(),
                      encoded.begin() + 13U);
            return WriteAtomically(path, encoded) ? NetworkIdentityError::kNone
                                                  : NetworkIdentityError::kWriteFailure;
        }
        if (!std::filesystem::is_regular_file(path, error) || error ||
            std::filesystem::file_size(path, error) != kMarkerSize || error) {
            return NetworkIdentityError::kMalformedMarker;
        }
        std::array<std::uint8_t, kMarkerSize> encoded{};
        std::ifstream input{path, std::ios::binary};
        if (!input.is_open()) {
            return NetworkIdentityError::kFilesystemFailure;
        }
        input.read(reinterpret_cast<char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size()));
        if (input.gcount() != static_cast<std::streamsize>(encoded.size()) || input.peek() != EOF) {
            return NetworkIdentityError::kMalformedMarker;
        }
        if (!std::equal(kMarkerMagic.begin(), kMarkerMagic.end(), encoded.begin()) ||
            ReadU32(encoded, 4U) != kMarkerVersion) {
            return NetworkIdentityError::kMalformedMarker;
        }
        if (encoded[8U] != static_cast<std::uint8_t>(network.id) ||
            ReadU32(encoded, 9U) != network.network_magic ||
            !std::equal(network.genesis_hash.bytes().begin(), network.genesis_hash.bytes().end(),
                        encoded.begin() + 13U)) {
            return NetworkIdentityError::kNetworkMismatch;
        }
        return NetworkIdentityError::kNone;
    } catch (...) {
        return NetworkIdentityError::kFilesystemFailure;
    }
}

} // namespace nova::storage
