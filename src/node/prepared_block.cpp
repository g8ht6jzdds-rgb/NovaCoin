#include "node/prepared_block.hpp"
#include "chain/mempool_image.hpp"
#include "node/generation_recovery.hpp"

#include "storage/block_journal.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace nova::node
{
namespace
{
bool SameSnapshot(const storage::GenerationSnapshot& a, const storage::GenerationSnapshot& b)
{
    return a.sequence == b.sequence && a.journal_size == b.journal_size &&
           a.wallet_size == b.wallet_size && a.journal_hash == b.journal_hash &&
           a.wallet_hash == b.wallet_hash && a.mempool.has_value() == b.mempool.has_value() &&
           a.mempool_size == b.mempool_size && a.mempool_hash == b.mempool_hash;
}

bool MatchesImage(const std::filesystem::path& path, const std::uint64_t expected_size,
                  const crypto::Hash256& expected_hash)
{
    using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Digest digest{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open() || !digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        return false;
    std::array<char, 65536U> bytes{};
    std::uint64_t count{};
    while (input) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto read = input.gcount();
        if (read < 0 || static_cast<std::uint64_t>(read) > expected_size - count)
            return false;
        if (read > 0 &&
            EVP_DigestUpdate(digest.get(), bytes.data(), static_cast<std::size_t>(read)) != 1)
            return false;
        count += static_cast<std::uint64_t>(read);
    }
    crypto::Hash256::Bytes hash{};
    unsigned int length{};
    return input.eof() && !input.bad() && count == expected_size &&
           EVP_DigestFinal_ex(digest.get(), hash.data(), &length) == 1 && length == hash.size() &&
           crypto::Hash256{hash} == expected_hash;
}
} // namespace

bool PreparedNodeBlock::CanPublish(const chain::ChainState& chain, const wallet::Wallet& wallet,
                                   const chain::Mempool& mempool, const std::uint32_t current_time,
                                   const wallet::Wallet::PreparedAddress* address) const noexcept
{
    if (current_time != prior_time_ || chain_ == nullptr || !chain.CanCommitPrepared(*chain_))
        return false;
    if (!result_.became_active)
        return address == nullptr && wallet_ == nullptr && mempool_ == nullptr;
    if (wallet_ == nullptr || mempool_ == nullptr || !mempool.CanCommitPrepared(*mempool_))
        return false;
    return address == nullptr ? wallet.CanCommitPrepared(*wallet_)
                              : wallet.CanCommitPreparedJoint(*wallet_, *address);
}

std::unique_ptr<PreparedNodeGeneration> PreparedNodeBlock::StageGeneration(
    storage::GenerationStore& store, const storage::GenerationSnapshot& expected,
    const chain::ChainState& chain, const wallet::Wallet& wallet, const chain::Mempool& mempool,
    const std::uint32_t current_time, const std::filesystem::path& fresh_directory,
    const primitives::BlockLimits limits, const std::span<const std::uint8_t> passphrase,
    GenerationStageError& error, const wallet::Wallet::PreparedAddress* address) const noexcept
{
    error = GenerationStageError::kStale;
    if (block_ == nullptr || !result_.block_hash.has_value() ||
        !CanPublish(chain, wallet, mempool, current_time, address))
        return nullptr;
    if (passphrase.empty()) {
        error = GenerationStageError::kWallet;
        return nullptr;
    }
    try {
        auto observed = store.Observe();
        if (observed.state != storage::GenerationState::kPresent ||
            !SameSnapshot(observed.snapshot, expected)) {
            error = GenerationStageError::kInvalidBase;
            return nullptr;
        }
        error = GenerationStageError::kWallet;
        if (!wallet.VerifyEncryptedImage(observed.snapshot.wallet, passphrase))
            return nullptr;
        error = GenerationStageError::kFilesystem;
        std::error_code fs_error;
        if (fresh_directory.empty() ||
            !std::filesystem::create_directory(fresh_directory, fs_error) || fs_error)
            return nullptr;
        const auto journal_path = fresh_directory / "blocks.dat";
        if (!std::filesystem::copy_file(observed.snapshot.journal, journal_path,
                                        std::filesystem::copy_options::none, fs_error) ||
            fs_error || !MatchesImage(journal_path, expected.journal_size, expected.journal_hash))
            return nullptr;
        error = GenerationStageError::kJournal;
        std::ifstream input{journal_path, std::ios::binary};
        if (!input.is_open())
            return nullptr;
        const auto decoded = storage::ReadBlockJournal(input, limits);
        input.close();
        if (decoded.error != storage::BlockJournalError::kNone || decoded.recovery_required)
            return nullptr;
        auto journal = storage::BlockJournal::Open(fresh_directory, limits);
        if (!journal.has_value() ||
            journal->Prepare(*block_) != storage::BlockJournalError::kNone ||
            journal->Commit(*result_.block_hash) != storage::BlockJournalError::kNone)
            return nullptr;
        error = GenerationStageError::kWallet;
        const auto wallet_path = fresh_directory / "wallet.dat";
        bool wallet_saved = false;
        if (!result_.became_active) {
            wallet_saved =
                std::filesystem::copy_file(observed.snapshot.wallet, wallet_path,
                                           std::filesystem::copy_options::none, fs_error) &&
                !fs_error && MatchesImage(wallet_path, expected.wallet_size, expected.wallet_hash);
        } else {
            wallet_saved = address == nullptr
                               ? wallet.SavePreparedEncrypted(*wallet_, wallet_path, passphrase)
                               : wallet.SavePreparedJointEncrypted(*wallet_, *address, wallet_path,
                                                                   passphrase);
        }
        if (!wallet_saved)
            return nullptr;
        observed = store.Observe();
        if (observed.state != storage::GenerationState::kPresent ||
            !SameSnapshot(observed.snapshot, expected) ||
            !CanPublish(chain, wallet, mempool, current_time, address)) {
            error = GenerationStageError::kStale;
            return nullptr;
        }
        error = GenerationStageError::kStore;
        storage::GenerationError store_error{};
        std::unique_ptr<storage::GenerationStore::Prepared> staged;
        if (expected.mempool.has_value()) {
            if (!result_.became_active) {
                staged = store.Prepare(journal_path, wallet_path, *expected.mempool, store_error);
            } else {
                const auto bytes =
                    chain::EncodePreparedMempoolImage(mempool, *mempool_, mempool.parameters());
                const auto records =
                    bytes ? chain::DecodeMempoolImage(*bytes, mempool.parameters()) : std::nullopt;
                const auto active = chain.GetPreparedActiveBlockIndexes(*chain_);
                if (!bytes || !records || !active)
                    return nullptr;
                std::vector<primitives::Transaction> pending;
                for (const auto& record : *records)
                    pending.push_back(record.transaction);
                const bool empty_anchor = !active->empty() && active->front().height == 0U &&
                                          !active->front().block.has_value();
                if (!wallet.MatchesPreparedReplay(*wallet_, *active, pending, empty_anchor,
                                                  address))
                    return nullptr;
                const auto path = fresh_directory / "mempool.dat";
                std::ofstream output{path, std::ios::binary | std::ios::trunc};
                output.write(reinterpret_cast<const char*>(bytes->data()),
                             static_cast<std::streamsize>(bytes->size()));
                output.close();
                if (!output)
                    return nullptr;
                staged = store.Prepare(journal_path, wallet_path, path, store_error);
                if (!staged)
                    return nullptr;
                const auto image = store.InspectPrepared(*staged);
                const auto digest = crypto::Hash256::Sha256(*bytes);
                if (!digest || image.state != storage::GenerationState::kPresent ||
                    image.snapshot.mempool_size != bytes->size() ||
                    image.snapshot.mempool_hash != *digest)
                    return nullptr;
                const wallet::WalletParams wallet_params{limits.transaction_limits, 1, 1U, true};
                const auto staged_wallet =
                    wallet::Wallet::LoadEncrypted(wallet_params, image.snapshot.wallet, passphrase);
                if (!staged_wallet ||
                    staged_wallet->NextReceivingAddressIndex() !=
                        (address == nullptr ? wallet.NextReceivingAddressIndex()
                                            : address->address().index + 1U) ||
                    !staged_wallet->MatchesFullReplay(*active, pending, empty_anchor))
                    return nullptr;
                auto addresses = wallet.ListReceivingAddresses();
                if (address != nullptr)
                    addresses.push_back(address->address());
                const auto actual = staged_wallet->ListReceivingAddresses();
                if (addresses.size() != actual.size())
                    return nullptr;
                for (const auto& expected_address : addresses) {
                    const auto found =
                        std::find_if(actual.begin(), actual.end(), [&](const auto& candidate) {
                            return candidate.index == expected_address.index &&
                                   candidate.public_key_hash == expected_address.public_key_hash &&
                                   candidate.public_key.SerializeCompressed() ==
                                       expected_address.public_key.SerializeCompressed();
                        });
                    if (found == actual.end())
                        return nullptr;
                }
            }
        } else {
            staged = store.Prepare(journal_path, wallet_path, store_error);
        }
        if (staged == nullptr)
            return nullptr;
        auto result = std::unique_ptr<PreparedNodeGeneration>{new PreparedNodeGeneration{}};
        result->owner_ = identity_;
        result->generation_ = std::move(staged);
        error = GenerationStageError::kNone;
        return result;
    } catch (...) {
        return nullptr;
    }
}

std::unique_ptr<PreparedNodeGeneration> PreparedNodeBlock::StageRecoveredGeneration(
    storage::GenerationStore& store, RecoveredGeneration& recovered, const chain::Mempool& mempool,
    const std::uint32_t current_time, const std::filesystem::path& fresh_directory,
    const std::span<const std::uint8_t> passphrase, GenerationStageError& error,
    const wallet::Wallet::PreparedAddress* address) const noexcept
{
    error = GenerationStageError::kStale;
    if (!recovered.MatchesStore(store) || !recovered.StillMatchesBase(current_time))
        return nullptr;
    return StageGeneration(store, recovered.snapshot(), recovered.chain(), recovered.wallet(),
                           mempool, current_time, fresh_directory, recovered.block_limits(),
                           passphrase, error, address);
}

NodePublicationResult
PreparedNodeBlock::CommitGeneration(PreparedNodeGeneration& generation,
                                    storage::GenerationStore& store, RecoveredGeneration& recovered,
                                    chain::Mempool& mempool,
                                    wallet::Wallet::PreparedAddress* address) noexcept
{
    if (generation.owner_.lock() != identity_ || generation.generation_ == nullptr ||
        generation.finished_)
        return NodePublicationResult::kStale;
    if (generation.publication_failed_)
        return NodePublicationResult::kUnavailable;
    auto& chain = recovered.chain();
    auto& wallet = recovered.wallet();
    const auto current_time = recovered.next_time();
    if (!recovered.MatchesStore(store) || !recovered.StillMatchesBase(current_time) ||
        !CanPublish(chain, wallet, mempool, current_time, address))
        return generation.attempted_ ? NodePublicationResult::kUnavailable
                                     : NodePublicationResult::kStale;
    storage::GenerationCommitResult durable;
    if (!generation.attempted_) {
        generation.attempted_ = true;
        durable = store.Commit(*generation.generation_);
    } else {
        durable = store.Resolve(*generation.generation_);
    }
    if (durable.state == storage::GenerationCommitState::kUnavailable)
        durable = store.Resolve(*generation.generation_);
    if (durable.state == storage::GenerationCommitState::kUnavailable) {
        recovered.Invalidate();
        return NodePublicationResult::kUnavailable;
    }
    if (durable.state == storage::GenerationCommitState::kNotCommitted) {
        generation.finished_ = true;
        return NodePublicationResult::kNotCommitted;
    }
    if (!CanPublish(chain, wallet, mempool, current_time, address) ||
        chain.CommitPrepared(*chain_).error != chain::ChainError::kNone ||
        (result_.became_active &&
         (!(address == nullptr ? wallet.CommitPreparedState(*wallet_)
                               : wallet.CommitPreparedJoint(*wallet_, *address)) ||
          !mempool.CommitPrepared(*mempool_)))) {
        generation.publication_failed_ = true;
        recovered.Invalidate();
        return NodePublicationResult::kUnavailable;
    }
    if (!recovered.AdvanceAfterPublication(store, next_time_)) {
        generation.publication_failed_ = true;
        return NodePublicationResult::kUnavailable;
    }
    generation.finished_ = true;
    return NodePublicationResult::kCommitted;
}

BlockPreparationResult PrepareNodeBlock(chain::ChainState& chain, wallet::Wallet& wallet,
                                        chain::Mempool& mempool, const primitives::Block& block,
                                        const std::span<const primitives::Transaction> known,
                                        const std::uint32_t next_time,
                                        const wallet::Wallet::PreparedAddress* address) noexcept
{
    if (block.header.time == std::numeric_limits<std::uint32_t>::max())
        return {BlockPreparationError::kContext, {}, nullptr};
    try {
        auto prepared = chain.PrepareBlock(block);
        if (prepared.result.error != chain::ChainError::kNone || prepared.prepared == nullptr)
            return {BlockPreparationError::kChain, prepared.result, nullptr};
        auto candidate = std::unique_ptr<PreparedNodeBlock>{new PreparedNodeBlock{}};
        candidate->identity_ = std::make_shared<const std::uint8_t>(0U);
        candidate->result_ = prepared.result;
        candidate->block_ = std::make_unique<primitives::Block>(block);
        candidate->prior_time_ = next_time;
        candidate->next_time_ = std::max(next_time, block.header.time + 1U);
        candidate->chain_ = std::move(prepared.prepared);
        if (!candidate->result_.became_active) {
            // A generated key must not be published for an inactive block.
            if (address != nullptr)
                return {BlockPreparationError::kContext, candidate->result_, nullptr};
            return {BlockPreparationError::kNone, candidate->result_, std::move(candidate)};
        }
        const auto active = chain.GetPreparedActiveBlockIndexes(*candidate->chain_);
        const auto previous_active = chain.GetActiveBlockIndexes();
        if (!active.has_value() || active->empty() || !previous_active.has_value() ||
            active->back().height == std::numeric_limits<std::uint32_t>::max())
            return {BlockPreparationError::kContext, candidate->result_, nullptr};
        std::vector<primitives::Transaction> confirmed;
        for (const auto& index : *active) {
            if (index.block.has_value())
                confirmed.insert(confirmed.end(), index.block->transactions.begin(),
                                 index.block->transactions.end());
        }
        std::size_t fork = 0U;
        while (fork < active->size() && fork < previous_active->size() &&
               (*active)[fork].hash == (*previous_active)[fork].hash)
            ++fork;
        std::vector<primitives::Transaction> candidates;
        for (std::size_t i = fork; i < previous_active->size(); ++i) {
            const auto& detached = (*previous_active)[i].block;
            if (!detached || detached->transactions.empty())
                return {BlockPreparationError::kContext, candidate->result_, nullptr};
            candidates.insert(candidates.end(), detached->transactions.begin() + 1,
                              detached->transactions.end());
        }
        candidates.insert(candidates.end(), known.begin(), known.end());
        chain::Mempool::PreparationResult pool_result;
        const auto visited =
            chain.WithPreparedUTXO(*candidate->chain_, [&](const chain::UTXOView& view) {
                pool_result = mempool.PrepareReconciliation(
                    confirmed, candidates, {active->back().height + 1U, candidate->next_time_},
                    view, candidate->next_time_);
                return pool_result.result.error == chain::MempoolError::kNone &&
                       pool_result.prepared != nullptr;
            });
        if (!visited) {
            const auto error = pool_result.result.error == chain::MempoolError::kNone
                                   ? BlockPreparationError::kContext
                                   : BlockPreparationError::kMempool;
            return {error, candidate->result_, nullptr};
        }
        candidate->mempool_ = std::move(pool_result.prepared);
        const auto image =
            chain::EncodePreparedMempoolImage(mempool, *candidate->mempool_, mempool.parameters());
        const auto records =
            image ? chain::DecodeMempoolImage(*image, mempool.parameters()) : std::nullopt;
        if (!records)
            return {BlockPreparationError::kMempool, candidate->result_, nullptr};
        std::vector<primitives::Transaction> pending;
        for (const auto& record : *records)
            pending.push_back(record.transaction);
        const bool empty_anchor =
            active->front().height == 0U && !active->front().block.has_value();
        candidate->wallet_ = wallet.PrepareReplayState(*active, pending, empty_anchor, address);
        if (candidate->wallet_ == nullptr)
            return {BlockPreparationError::kWallet, candidate->result_, nullptr};
        if (!candidate->CanPublish(chain, wallet, mempool, next_time, address))
            return {BlockPreparationError::kContext, candidate->result_, nullptr};
        return {BlockPreparationError::kNone, candidate->result_, std::move(candidate)};
    } catch (...) {
        return {BlockPreparationError::kAllocation, {}, nullptr};
    }
}

} // namespace nova::node
