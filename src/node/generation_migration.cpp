#include "node/generation_migration.hpp"

#include "node/network_selection.hpp"

#include <utility>

namespace nova::node
{
namespace
{
bool SameImages(const storage::GenerationSnapshot& a, const storage::GenerationSnapshot& b)
{
    return a.journal_size == b.journal_size && a.wallet_size == b.wallet_size &&
           a.journal_hash == b.journal_hash && a.wallet_hash == b.wallet_hash &&
           a.mempool.has_value() == b.mempool.has_value() && a.mempool_size == b.mempool_size &&
           a.mempool_hash == b.mempool_hash;
}
} // namespace

MigrationPrepareResult
PrepareLegacyMigration(storage::GenerationStore& store, const consensus::NetworkId network,
                       const std::shared_ptr<const chain::ValidationTimeSource>& time_source,
                       const std::uint32_t start_time,
                       const std::span<const std::uint8_t> passphrase,
                       const std::filesystem::path& journal, const std::filesystem::path& wallet,
                       const std::filesystem::path& fresh_validation_directory) noexcept
{
    const auto selected = SelectNetwork(network);
    if (selected.error != NetworkSelectionError::kNone || selected.parameters == nullptr ||
        !store.MatchesNetwork(*selected.parameters) || time_source == nullptr)
        return {MigrationError::kNetwork, {}, nullptr};
    if (passphrase.empty())
        return {MigrationError::kValidation, GenerationRecoveryError::kWallet, nullptr};
    try {
        const auto initial = store.Observe();
        if (initial.state == storage::GenerationState::kUnavailable)
            return {MigrationError::kUnavailable, {}, nullptr};
        if (initial.state != storage::GenerationState::kEmpty)
            return {MigrationError::kNotEmpty, {}, nullptr};
        storage::GenerationError error{};
        auto candidate = store.Prepare(journal, wallet, error);
        if (candidate == nullptr)
            return {MigrationError::kPrepare, {}, nullptr};
        const auto staged = store.InspectPrepared(*candidate);
        if (staged.state != storage::GenerationState::kPresent)
            return {MigrationError::kPrepare, {}, nullptr};
        std::error_code fs_error;
        if (fresh_validation_directory.empty() ||
            !std::filesystem::create_directory(fresh_validation_directory, fs_error) || fs_error)
            return {MigrationError::kValidation, {}, nullptr};
        auto validation_store =
            storage::GenerationStore::Open(fresh_validation_directory, *selected.parameters, error);
        if (validation_store == nullptr)
            return {MigrationError::kValidation, {}, nullptr};
        auto validation =
            validation_store->Prepare(staged.snapshot.journal, staged.snapshot.wallet, error);
        if (validation == nullptr || validation_store->Commit(*validation).state !=
                                         storage::GenerationCommitState::kCommitted)
            return {MigrationError::kValidation, {}, nullptr};
        const auto validation_image = validation_store->Observe();
        if (validation_image.state != storage::GenerationState::kPresent ||
            !SameImages(staged.snapshot, validation_image.snapshot))
            return {MigrationError::kValidation, {}, nullptr};
        auto checked =
            RecoverGeneration(*validation_store, network, time_source, start_time, passphrase);
        if (checked.error != GenerationRecoveryError::kNone || checked.recovered == nullptr)
            return {MigrationError::kValidation, checked.error, nullptr};
        const auto again = store.InspectPrepared(*candidate);
        if (again.state != storage::GenerationState::kPresent ||
            !SameImages(staged.snapshot, again.snapshot) ||
            store.Observe().state != storage::GenerationState::kEmpty ||
            store.Resolve(*candidate).state != storage::GenerationCommitState::kNotCommitted)
            return {MigrationError::kStale, {}, nullptr};
        auto result = std::unique_ptr<PreparedMigration>{new PreparedMigration{}};
        result->owner_ = store.CaptureOwnerCheckpoint();
        result->snapshot_ = staged.snapshot;
        result->prepared_ = std::move(candidate);
        result->network_ = network;
        result->start_time_ = start_time;
        return {MigrationError::kNone, GenerationRecoveryError::kNone, std::move(result)};
    } catch (...) {
        return {MigrationError::kUnavailable, {}, nullptr};
    }
}

MigrationCommitResult
CommitLegacyMigration(PreparedMigration& migration, storage::GenerationStore& store,
                      const consensus::NetworkId network,
                      std::shared_ptr<const chain::ValidationTimeSource> time_source,
                      const std::uint32_t start_time,
                      const std::span<const std::uint8_t> passphrase) noexcept
{
    if (migration.finished_ || !store.IsOwnerCheckpointCurrent(migration.owner_) ||
        migration.network_ != network || migration.start_time_ != start_time ||
        time_source == nullptr || passphrase.empty() || migration.prepared_ == nullptr)
        return {};
    if (!migration.attempted_) {
        const auto staged = store.InspectPrepared(*migration.prepared_);
        if (staged.state != storage::GenerationState::kPresent ||
            !SameImages(staged.snapshot, migration.snapshot_) ||
            store.Observe().state != storage::GenerationState::kEmpty)
            return {};
        const auto selected = SelectNetwork(network);
        if (selected.error != NetworkSelectionError::kNone || selected.parameters == nullptr ||
            !store.MatchesNetwork(*selected.parameters))
            return {};
        const wallet::WalletParams params{selected.parameters->block_limits.transaction_limits, 1,
                                          1U, true};
        if (wallet::Wallet::LoadEncrypted(params, staged.snapshot.wallet, passphrase) == nullptr)
            return {storage::GenerationCommitState::kNotCommitted, nullptr};
    }
    auto outcome = migration.attempted_ ? store.Resolve(*migration.prepared_)
                                        : store.Commit(*migration.prepared_);
    migration.attempted_ = true;
    if (outcome.state == storage::GenerationCommitState::kUnavailable)
        outcome = store.Resolve(*migration.prepared_);
    if (outcome.state == storage::GenerationCommitState::kNotCommitted) {
        migration.finished_ = true;
        return {outcome.state, nullptr};
    }
    if (outcome.state != storage::GenerationCommitState::kCommitted)
        return {};
    auto recovered =
        RecoverGeneration(store, network, std::move(time_source), start_time, passphrase);
    if (recovered.error != GenerationRecoveryError::kNone || recovered.recovered == nullptr)
        return {};
    migration.finished_ = true;
    return {storage::GenerationCommitState::kCommitted, std::move(recovered.recovered)};
}
} // namespace nova::node
