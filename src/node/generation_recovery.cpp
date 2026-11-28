#include "node/generation_recovery.hpp"

#include "node/network_selection.hpp"
#include "storage/block_journal.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <utility>

namespace nova::node
{

std::unique_ptr<chain::Mempool>
ValidateRecoveredMempool(std::istream& input, const chain::MempoolParams& parameters,
                         const chain::TransactionValidationContext& context,
                         const chain::UTXOView& utxos, const wallet::Wallet& wallet,
                         const std::span<const chain::BlockIndex> active,
                         const bool allow_empty_anchor) noexcept
{
    try {
        const auto bytes = chain::ReadMempoolImage(input, parameters);
        if (!bytes)
            return nullptr;
        const auto records = chain::DecodeMempoolImage(*bytes, parameters);
        if (!records)
            return nullptr;
        auto pool = chain::ReplayMempoolImage(*bytes, parameters, context, utxos);
        if (!pool)
            return nullptr;
        std::vector<primitives::Transaction> pending;
        pending.reserve(records->size());
        for (const auto& record : *records)
            pending.push_back(record.transaction);
        if (!wallet.MatchesFullReplay(active, pending, allow_empty_anchor) || !utxos.IsCurrent())
            return nullptr;
        return pool;
    } catch (...) {
        return nullptr;
    }
}

bool RecoveredGeneration::StillMatchesBase(const std::uint32_t current_time) const noexcept
{
    return valid_ && current_time == next_time_ && network_chain_.chain != nullptr &&
           network_chain_.utxos != nullptr && wallet_ != nullptr && utxo_checkpoint_ != nullptr &&
           utxo_checkpoint_->IsCurrent() &&
           network_chain_.chain->IsCheckpointCurrent(chain_checkpoint_) &&
           wallet_->IsCheckpointCurrent(wallet_checkpoint_);
}

bool RecoveredGeneration::AdvanceAfterPublication(storage::GenerationStore& store,
                                                  const std::uint32_t next_time) noexcept
{
    valid_ = false;
    if (std::exchange(fail_next_advance_, false))
        return false;
    try {
        if (!MatchesStore(store))
            return false;
        auto observed = store.Observe();
        if (observed.state != storage::GenerationState::kPresent ||
            observed.snapshot.sequence <= snapshot_.sequence)
            return false;
        auto batch = network_chain_.utxos->BeginBatch();
        if (!batch.has_value())
            return false;
        auto checkpoint = std::make_unique<chain::UTXOBatch>(std::move(*batch));
        auto chain_checkpoint = network_chain_.chain->CaptureCheckpoint();
        auto wallet_checkpoint = wallet_->CaptureCheckpoint();
        snapshot_ = std::move(observed.snapshot);
        chain_checkpoint_ = std::move(chain_checkpoint);
        wallet_checkpoint_ = std::move(wallet_checkpoint);
        utxo_checkpoint_ = std::move(checkpoint);
        next_time_ = next_time;
        valid_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool RecoveredGeneration::MatchesStore(const storage::GenerationStore& store) const noexcept
{
    return store.IsOwnerCheckpointCurrent(store_checkpoint_);
}

GenerationRecoveryResult
RecoverGeneration(storage::GenerationStore& store, const consensus::NetworkId id,
                  std::shared_ptr<const chain::ValidationTimeSource> time_source,
                  const std::uint32_t start_time, const std::span<const std::uint8_t> passphrase,
                  const GenerationRecoveryObserver observer) noexcept
{
    const auto selected = SelectNetwork(id);
    if (selected.error != NetworkSelectionError::kNone || selected.parameters == nullptr ||
        !store.MatchesNetwork(*selected.parameters) || time_source == nullptr ||
        start_time < selected.parameters->genesis_block.header.time)
        return {GenerationRecoveryError::kNetwork, nullptr};
    if (passphrase.empty())
        return {GenerationRecoveryError::kWallet, nullptr};
    try {
        const auto before = store.Observe();
        if (before.state != storage::GenerationState::kPresent)
            return {GenerationRecoveryError::kUnavailable, nullptr};
        std::ifstream stream{before.snapshot.journal, std::ios::binary};
        if (!stream.is_open())
            return {GenerationRecoveryError::kJournal, nullptr};
        const auto journal = storage::ReadBlockJournal(stream, selected.parameters->block_limits);
        if (journal.error != storage::BlockJournalError::kNone || journal.recovery_required)
            return {GenerationRecoveryError::kJournal, nullptr};
        auto initialized = chain::NetworkChainInitializer::Create(id, std::move(time_source));
        if (!initialized.has_value())
            return {GenerationRecoveryError::kChain, nullptr};
        auto next_time = start_time;
        std::size_t completed{};
        const auto total = journal.blocks.size();
        observer.Notify(GenerationRecoveryCheckpoint::kBeforeReplay, completed, total);
        for (const auto& block : journal.blocks) {
            if (block.header.time == std::numeric_limits<std::uint32_t>::max())
                return {GenerationRecoveryError::kContext, nullptr};
            if (initialized->chain->AcceptBlock(block).error != chain::ChainError::kNone)
                return {GenerationRecoveryError::kChain, nullptr};
            next_time = std::max(next_time, block.header.time + 1U);
            observer.Notify(GenerationRecoveryCheckpoint::kAfterBlock, ++completed, total);
        }
        const wallet::WalletParams params{selected.parameters->block_limits.transaction_limits, 1,
                                          1U, true};
        observer.Notify(GenerationRecoveryCheckpoint::kBeforeWallet, completed, total);
        auto wallet = wallet::Wallet::LoadEncrypted(params, before.snapshot.wallet, passphrase);
        if (wallet == nullptr)
            return {GenerationRecoveryError::kWallet, nullptr};
        observer.Notify(GenerationRecoveryCheckpoint::kAfterWallet, completed, total);
        const auto active = initialized->chain->GetActiveBlockIndexes();
        if (!active.has_value())
            return {GenerationRecoveryError::kChain, nullptr};
        const chain::MempoolParams mempool_params{
            chain::NetworkValidationParams(*selected.parameters), 1000U, 10000000U, 0};
        std::unique_ptr<chain::Mempool> mempool;
        observer.Notify(GenerationRecoveryCheckpoint::kBeforeMempool, completed, total);
        if (before.snapshot.mempool.has_value()) {
            if (initialized->chain->active_height() == std::numeric_limits<std::uint32_t>::max())
                return {GenerationRecoveryError::kContext, nullptr};
            auto view = initialized->utxos->BeginBatch();
            std::ifstream input{*before.snapshot.mempool, std::ios::binary};
            if (!view || !input.is_open())
                return {GenerationRecoveryError::kUnavailable, nullptr};
            mempool = ValidateRecoveredMempool(
                input, mempool_params, {initialized->chain->active_height() + 1U, next_time}, *view,
                *wallet, *active, id == consensus::NetworkId::kRegtest);
            if (!mempool)
                return {GenerationRecoveryError::kUnavailable, nullptr};
        } else {
            if (!wallet->MatchesFullReplay(*active, {}, id == consensus::NetworkId::kRegtest))
                return {GenerationRecoveryError::kWallet, nullptr};
            mempool = chain::Mempool::Create(mempool_params);
            if (!mempool)
                return {GenerationRecoveryError::kUnavailable, nullptr};
        }
        observer.Notify(GenerationRecoveryCheckpoint::kAfterMempool, completed, total);
        const auto after = store.Observe();
        if (after.state != storage::GenerationState::kPresent ||
            after.snapshot.sequence != before.snapshot.sequence ||
            after.snapshot.journal_size != before.snapshot.journal_size ||
            after.snapshot.wallet_size != before.snapshot.wallet_size ||
            after.snapshot.journal_hash != before.snapshot.journal_hash ||
            after.snapshot.wallet_hash != before.snapshot.wallet_hash ||
            after.snapshot.mempool.has_value() != before.snapshot.mempool.has_value() ||
            after.snapshot.mempool_size != before.snapshot.mempool_size ||
            after.snapshot.mempool_hash != before.snapshot.mempool_hash)
            return {GenerationRecoveryError::kUnavailable, nullptr};
        auto utxo_checkpoint = initialized->utxos->BeginBatch();
        if (!utxo_checkpoint.has_value())
            return {GenerationRecoveryError::kContext, nullptr};
        auto recovered = std::unique_ptr<RecoveredGeneration>{new RecoveredGeneration{}};
        recovered->snapshot_ = before.snapshot;
        recovered->chain_checkpoint_ = initialized->chain->CaptureCheckpoint();
        recovered->wallet_checkpoint_ = wallet->CaptureCheckpoint();
        recovered->store_checkpoint_ = store.CaptureOwnerCheckpoint();
        recovered->utxo_checkpoint_ =
            std::make_unique<chain::UTXOBatch>(std::move(*utxo_checkpoint));
        recovered->network_chain_ = std::move(*initialized);
        recovered->wallet_ = std::move(wallet);
        recovered->next_time_ = next_time;
        recovered->block_limits_ = selected.parameters->block_limits;
        observer.Notify(GenerationRecoveryCheckpoint::kBeforeAdmission, completed, total);
        return {GenerationRecoveryError::kNone, std::move(recovered), std::move(mempool)};
    } catch (...) {
        return {GenerationRecoveryError::kUnavailable, nullptr};
    }
}
} // namespace nova::node
