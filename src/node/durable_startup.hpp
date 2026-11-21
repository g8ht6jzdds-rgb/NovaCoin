#pragma once

#include "node/data_directory_lease.hpp"
#include "node/durable_session.hpp"
#include "node/staging_workspace.hpp"
#include "storage/network_identity.hpp"

namespace nova::node
{
// Node-level startup ownership, independent of RPC and network listeners.
// Declaration order keeps the directory lease alive until state is destroyed.
struct DurableNodeState final {
    std::unique_ptr<storage::GenerationStore> directory_lease;
    std::unique_ptr<StagingWorkspace> workspace;
    std::unique_ptr<DurableSession> session;
};

// Existing legacy files are refused, never silently migrated or overwritten.
// Callers must retain the returned lease for the entire lifetime of the session.
[[nodiscard]] inline std::unique_ptr<DurableNodeState>
OpenDurableNodeState(const std::filesystem::path& directory, consensus::NetworkId network_id,
                     const std::shared_ptr<const chain::ValidationTimeSource>& time_source,
                     std::uint32_t start_time, std::span<const std::uint8_t> passphrase) noexcept
{
    if (!time_source || passphrase.empty() ||
        (network_id != consensus::NetworkId::kRegtest &&
         network_id != consensus::NetworkId::kTestnet))
        return nullptr;
    try {
        const auto& network = consensus::GetNetworkParams(network_id);
        std::error_code error;
        const auto initial_status = std::filesystem::symlink_status(directory, error);
        if ((error && error != std::errc::no_such_file_or_directory) ||
            (!error && std::filesystem::is_symlink(initial_status)))
            return nullptr;
        error.clear();
        auto directory_lease = AcquireDataDirectoryLease(directory, network);
        if (!directory_lease)
            return nullptr;
        if (storage::EnsureNetworkIdentityWithLease(directory, network) !=
            storage::NetworkIdentityError::kNone)
            return nullptr;
        for (const auto* name : {"wallet.dat", "blocks.dat"}) {
            const auto status = std::filesystem::symlink_status(directory / name, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return nullptr;
            if (!error && std::filesystem::exists(status))
                return nullptr;
            error.clear();
        }
        const auto store_path = directory / "generations";
        const auto staging_path = directory / "staging";
        const bool fresh = std::filesystem::create_directory(store_path, error);
        if (error)
            return nullptr;
        const auto store_status = std::filesystem::symlink_status(store_path, error);
        if (error || !std::filesystem::is_directory(store_status) ||
            std::filesystem::is_symlink(store_status))
            return nullptr;
        std::filesystem::create_directory(staging_path, error);
        if (error)
            return nullptr;
        auto workspace = StagingWorkspace::Open(staging_path);
        if (!workspace)
            return nullptr;
        storage::GenerationError store_error{};
        auto store = storage::GenerationStore::Open(store_path, network, store_error);
        if (!store)
            return nullptr;
        const auto observation = store->Observe();
        std::unique_ptr<DurableSession> session;
        if (fresh && observation.state == storage::GenerationState::kEmpty) {
            const auto path = workspace->Next();
            if (!path)
                return nullptr;
            session = DurableSession::CreateFresh(std::move(store), network.id, time_source,
                                                  start_time, passphrase, *path);
            if (!workspace->Cleanup(*path))
                return nullptr;
        } else if (!fresh && observation.state == storage::GenerationState::kPresent) {
            session = DurableSession::Open(std::move(store), network.id, time_source, start_time,
                                           passphrase);
        }
        if (!session)
            return nullptr;
        auto opened = std::make_unique<DurableNodeState>();
        opened->directory_lease = std::move(directory_lease);
        opened->workspace = std::move(workspace);
        opened->session = std::move(session);
        return opened;
    } catch (...) {
        return nullptr;
    }
}
} // namespace nova::node
