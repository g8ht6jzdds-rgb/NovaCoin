#pragma once

#include "daemon/durable_startup.hpp"
#include "node/generation_migration.hpp"

namespace nova::daemon
{
// Copies an offline legacy REGTEST or TESTNET into a newly created destination. Failure
// leaves both the source and any partial destination in place for inspection.
// Legacy pending transactions are not persisted by the old node.
[[nodiscard]] inline std::unique_ptr<DurableRuntime>
MigrateDurableNode(const std::filesystem::path& source, const std::filesystem::path& destination,
                   const std::shared_ptr<const chain::ValidationTimeSource>& time_source,
                   const std::uint32_t start_time, const std::span<const std::uint8_t> passphrase,
                   rpc::RpcConfig config) noexcept
{
    if (!time_source || passphrase.empty() ||
        (config.network != consensus::NetworkId::kRegtest &&
         config.network != consensus::NetworkId::kTestnet) ||
        source.empty() || destination.empty())
        return nullptr;
    try {
        std::error_code error;
        const auto source_path = std::filesystem::canonical(source, error);
        if (error)
            return nullptr;
        const auto parent = std::filesystem::canonical(destination.parent_path(), error);
        if (error || destination.filename().empty() || destination.filename() == "." ||
            destination.filename() == "..")
            return nullptr;
        const auto target = parent / destination.filename();
        // Compare filesystem identity, not case-sensitive spelling (Windows
        // aliases may name the same source directory with different casing).
        for (auto ancestor = parent; !ancestor.empty();) {
            if (std::filesystem::equivalent(source_path, ancestor, error) || error)
                return nullptr;
            const auto next = ancestor.parent_path();
            if (next == ancestor)
                break;
            ancestor = next;
        }
        // Source identity and both legacy images must already exist. Do not
        // invent an identity or adopt mirrors beside another generation owner.
        const auto legacy_source_is_complete = [&source_path]() {
            std::error_code source_error;
            for (const auto* filename : {"network.identity", "wallet.dat", "blocks.dat"}) {
                const auto status =
                    std::filesystem::symlink_status(source_path / filename, source_error);
                if (source_error || !std::filesystem::is_regular_file(status) ||
                    std::filesystem::is_symlink(status))
                    return false;
            }
            const auto generations =
                std::filesystem::symlink_status(source_path / "generations", source_error);
            return source_error == std::errc::no_such_file_or_directory ||
                   (!source_error && !std::filesystem::exists(generations));
        };
        if (!legacy_source_is_complete())
            return nullptr;
        const auto& network = consensus::GetNetworkParams(config.network);
        auto source_lease = node::AcquireDataDirectoryLease(source_path, network);
        if (!source_lease || !legacy_source_is_complete() ||
            storage::EnsureNetworkIdentityWithLease(source_path, network) !=
                storage::NetworkIdentityError::kNone)
            return nullptr;
        if (!std::filesystem::create_directory(target, error) || error)
            return nullptr;
        auto destination_lease = node::AcquireDataDirectoryLease(target, network);
        if (!destination_lease || storage::EnsureNetworkIdentityWithLease(target, network) !=
                                      storage::NetworkIdentityError::kNone)
            return nullptr;
        if (!std::filesystem::create_directory(target / "generations", error) || error ||
            !std::filesystem::create_directory(target / "staging", error) || error)
            return nullptr;
#ifndef _WIN32
        if (!storage::SyncDirectoryHierarchy(target))
            return nullptr;
#endif
        auto workspace = node::StagingWorkspace::Open(target / "staging");
        storage::GenerationError store_error{};
        auto store = storage::GenerationStore::Open(target / "generations", network, store_error);
        if (!workspace || !store)
            return nullptr;
        auto prepared = node::PrepareLegacyMigration(
            *store, network.id, time_source, start_time, passphrase, source_path / "blocks.dat",
            source_path / "wallet.dat", target / "staging" / "migration-validation");
        if (prepared.error != node::MigrationError::kNone || !prepared.prepared)
            return nullptr;
        auto committed = node::CommitLegacyMigration(*prepared.prepared, *store, network.id,
                                                     time_source, start_time, passphrase);
        if (committed.state != storage::GenerationCommitState::kCommitted || !committed.recovered)
            return nullptr;
        committed.recovered.reset();
        prepared.prepared.reset();
        auto session = node::DurableSession::Open(std::move(store), network.id, time_source,
                                                  start_time, passphrase);
        return session
                   ? DurableRuntime::Create(std::move(session), std::move(workspace), passphrase,
                                            std::move(config), std::move(destination_lease))
                   : nullptr;
    } catch (...) {
        return nullptr;
    }
}
[[nodiscard]] inline std::unique_ptr<DurableRuntime>
MigrateDurableRegtest(const std::filesystem::path& source, const std::filesystem::path& destination,
                      const std::shared_ptr<const chain::ValidationTimeSource>& time_source,
                      std::uint32_t start_time, std::span<const std::uint8_t> passphrase,
                      rpc::RpcConfig config) noexcept
{
    if (config.network != consensus::NetworkId::kRegtest)
        return nullptr;
    return MigrateDurableNode(source, destination, time_source, start_time, passphrase,
                              std::move(config));
}

[[nodiscard]] inline std::unique_ptr<DurableRuntime>
MigrateDurableTestnet(const std::filesystem::path& source, const std::filesystem::path& destination,
                      const std::shared_ptr<const chain::ValidationTimeSource>& time_source,
                      std::uint32_t start_time, std::span<const std::uint8_t> passphrase,
                      rpc::RpcConfig config) noexcept
{
    if (config.network != consensus::NetworkId::kTestnet)
        return nullptr;
    return MigrateDurableNode(source, destination, time_source, start_time, passphrase,
                              std::move(config));
}
} // namespace nova::daemon
