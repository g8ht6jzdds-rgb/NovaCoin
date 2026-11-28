#pragma once

#include "storage/generation_store.hpp"

namespace nova::node
{
// Both node storage modes must retain this lease until their state is destroyed.
// The directory must be private to the node operator; older binaries do not
// participate in this protocol and must not access it concurrently.
[[nodiscard]] inline std::unique_ptr<storage::GenerationStore>
AcquireDataDirectoryLease(const std::filesystem::path& directory,
                          const consensus::NetworkParams& network) noexcept
{
    try {
        if (directory.empty())
            return nullptr;
        std::error_code error;
        const auto initial = std::filesystem::symlink_status(directory, error);
        if ((error && error != std::errc::no_such_file_or_directory) ||
            (!error && std::filesystem::is_symlink(initial)))
            return nullptr;
        error.clear();
        std::filesystem::create_directories(directory, error);
        if (error)
            return nullptr;
        const auto lock_directory = directory / ".node-lease";
        std::filesystem::create_directory(lock_directory, error);
        if (error)
            return nullptr;
        const auto status = std::filesystem::symlink_status(lock_directory, error);
        if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
            return nullptr;
        const auto absolute_lock_directory = std::filesystem::canonical(lock_directory, error);
        if (error)
            return nullptr;
        storage::GenerationError store_error{};
        auto lease = storage::GenerationStore::Open(absolute_lock_directory, network, store_error);
        if (!lease || lease->Observe().state != storage::GenerationState::kEmpty)
            return nullptr;
        return lease;
    } catch (...) {
        return nullptr;
    }
}
} // namespace nova::node
