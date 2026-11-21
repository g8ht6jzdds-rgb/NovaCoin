#pragma once

#include "node/generation_recovery.hpp"

namespace nova::node
{

enum class MigrationError : std::uint8_t {
    kNone,
    kNetwork,
    kNotEmpty,
    kPrepare,
    kValidation,
    kStale,
    kUnavailable
};
class PreparedMigration;
struct MigrationPrepareResult final {
    MigrationError error{MigrationError::kUnavailable};
    GenerationRecoveryError validation_error{GenerationRecoveryError::kNone};
    std::unique_ptr<PreparedMigration> prepared;
};
struct MigrationCommitResult final {
    storage::GenerationCommitState state{storage::GenerationCommitState::kUnavailable};
    std::unique_ptr<RecoveredGeneration> recovered;
};

// Copies existing files without rewriting them. A fresh private validation
// directory is required; no live node or service may concurrently write the
// source files or either store. This is not wired into node startup.
[[nodiscard]] MigrationPrepareResult
PrepareLegacyMigration(storage::GenerationStore& store, consensus::NetworkId network,
                       const std::shared_ptr<const chain::ValidationTimeSource>& time_source,
                       std::uint32_t start_time, std::span<const std::uint8_t> passphrase,
                       const std::filesystem::path& journal, const std::filesystem::path& wallet,
                       const std::filesystem::path& fresh_validation_directory) noexcept;

[[nodiscard]] MigrationCommitResult
CommitLegacyMigration(PreparedMigration& migration, storage::GenerationStore& store,
                      consensus::NetworkId network,
                      std::shared_ptr<const chain::ValidationTimeSource> time_source,
                      std::uint32_t start_time, std::span<const std::uint8_t> passphrase) noexcept;

class PreparedMigration final
{
  public:
    PreparedMigration(const PreparedMigration&) = delete;
    PreparedMigration& operator=(const PreparedMigration&) = delete;

  private:
    PreparedMigration() = default;
    storage::GenerationStore::OwnerCheckpoint owner_;
    storage::GenerationSnapshot snapshot_;
    std::unique_ptr<storage::GenerationStore::Prepared> prepared_;
    consensus::NetworkId network_{};
    std::uint32_t start_time_{};
    bool attempted_{};
    bool finished_{};
    friend MigrationPrepareResult
    PrepareLegacyMigration(storage::GenerationStore&, consensus::NetworkId,
                           const std::shared_ptr<const chain::ValidationTimeSource>&, std::uint32_t,
                           std::span<const std::uint8_t>, const std::filesystem::path&,
                           const std::filesystem::path&, const std::filesystem::path&) noexcept;
    friend MigrationCommitResult
    CommitLegacyMigration(PreparedMigration&, storage::GenerationStore&, consensus::NetworkId,
                          std::shared_ptr<const chain::ValidationTimeSource>, std::uint32_t,
                          std::span<const std::uint8_t>) noexcept;
};
} // namespace nova::node
