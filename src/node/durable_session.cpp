#include "node/durable_session.hpp"

#include <cstdio>

#include "chain/mempool_image.hpp"
#include "consensus/monetary.hpp"
#include "consensus/pow.hpp"
#include "node/mining_helpers.hpp"
#include "node/network_selection.hpp"
#include "storage/block_journal.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace nova::node
{
namespace
{
std::vector<std::uint8_t> P2pkhScript(const crypto::Hash160& hash)
{
    std::vector<std::uint8_t> script{0x76U, 0xA9U, 0x14U};
    script.insert(script.end(), hash.begin(), hash.end());
    script.insert(script.end(), {0x88U, 0xACU});
    return script;
}

} // namespace

DurableSession::DurableSession(std::unique_ptr<storage::GenerationStore> store,
                               std::unique_ptr<RecoveredGeneration> recovered,
                               std::unique_ptr<chain::Mempool> mempool,
                               const consensus::NetworkParams& network) noexcept
    : store_(std::move(store)), recovered_(std::move(recovered)), mempool_(std::move(mempool)),
      network_(&network)
{
}

std::unique_ptr<DurableSession> DurableSession::Open(
    std::unique_ptr<storage::GenerationStore> store, const consensus::NetworkId network,
    std::shared_ptr<const chain::ValidationTimeSource> time_source, const std::uint32_t start_time,
    const std::span<const std::uint8_t> passphrase) noexcept
{
    if (store == nullptr)
        return nullptr;
    const auto selected = SelectNetwork(network);
    if (selected.error != NetworkSelectionError::kNone || selected.parameters == nullptr ||
        !store->MatchesNetwork(*selected.parameters))
        return nullptr;
    try {
        auto recovered =
            RecoverGeneration(*store, network, std::move(time_source), start_time, passphrase);
        if (recovered.error != GenerationRecoveryError::kNone || recovered.recovered == nullptr)
            return nullptr;
        auto mempool = std::move(recovered.mempool);
        if (mempool == nullptr)
            return nullptr;
        return std::unique_ptr<DurableSession>{
            new DurableSession{std::move(store), std::move(recovered.recovered), std::move(mempool),
                               *selected.parameters}};
    } catch (...) {
        return nullptr;
    }
}

std::unique_ptr<DurableSession> DurableSession::CreateFresh(
    std::unique_ptr<storage::GenerationStore> store, const consensus::NetworkId network,
    std::shared_ptr<const chain::ValidationTimeSource> time_source, const std::uint32_t start_time,
    const std::span<const std::uint8_t> passphrase, const std::filesystem::path& directory) noexcept
{
    const auto selected = SelectNetwork(network);
    if (!store || selected.error != NetworkSelectionError::kNone || !selected.parameters ||
        !store->MatchesNetwork(*selected.parameters) || !time_source || passphrase.empty() ||
        start_time < selected.parameters->genesis_block.header.time ||
        store->Observe().state != storage::GenerationState::kEmpty)
        return nullptr;
    try {
        std::error_code error;
        if (directory.empty() || !std::filesystem::create_directory(directory, error) || error)
            return nullptr;
        const auto journal = directory / "blocks.dat";
        std::ofstream journal_file{journal, std::ios::binary};
        journal_file.close();
        if (!journal_file)
            return nullptr;
        const wallet::WalletParams wallet_params{
            selected.parameters->block_limits.transaction_limits, 1, 1U, true};
        auto wallet = wallet::Wallet::Create(wallet_params);
        const auto wallet_path = directory / "wallet.dat";
        if (!wallet || !wallet->SaveEncrypted(wallet_path, passphrase))
            return nullptr;
        const chain::MempoolParams pool_params{chain::NetworkValidationParams(*selected.parameters),
                                               1000U, 10000000U, 0};
        const auto image = chain::EncodeMempoolImage({}, pool_params);
        if (!image)
            return nullptr;
        const auto image_path = directory / "mempool.dat";
        std::ofstream image_file{image_path, std::ios::binary};
        image_file.write(reinterpret_cast<const char*>(image->data()),
                         static_cast<std::streamsize>(image->size()));
        image_file.close();
        if (!image_file)
            return nullptr;
        storage::GenerationError store_error{};
        auto prepared = store->Prepare(journal, wallet_path, image_path, store_error);
        if (!prepared)
            return nullptr;
        auto outcome = store->Commit(*prepared);
        if (outcome.state == storage::GenerationCommitState::kUnavailable)
            outcome = store->Resolve(*prepared);
        if (outcome.state != storage::GenerationCommitState::kCommitted)
            return nullptr;
        return Open(std::move(store), network, std::move(time_source), start_time, passphrase);
    } catch (...) {
        return nullptr;
    }
}

std::optional<DurableSessionView> DurableSession::View() const noexcept
{
    if (quarantined_ || recovered_ == nullptr ||
        !recovered_->StillMatchesBase(recovered_->next_time()) || !AuthorityMatches())
        return std::nullopt;
    const auto metadata = recovered_->chain().GetHeaderMetadata(recovered_->chain().active_tip());
    if (!metadata)
        return std::nullopt;
    return DurableSessionView{recovered_->chain().active_tip(),
                              recovered_->chain().active_height(),
                              recovered_->next_time(),
                              recovered_->wallet().ConfirmedBalance(),
                              mempool_->size(),
                              recovered_->wallet().ReceivingAddressCount(),
                              metadata->chain_work,
                              mempool_->total_serialized_size()};
}

std::optional<std::vector<primitives::Block>> DurableSession::CommittedBlocks() const noexcept
{
    if (!AuthorityMatches())
        return std::nullopt;
    try {
        std::ifstream input{recovered_->snapshot().journal, std::ios::binary};
        if (!input)
            return std::nullopt;
        storage::JournalReadBudget budget;
        budget.bytes = recovered_->snapshot().journal_size;
        auto loaded = storage::ReadBlockJournal(input, network_->block_limits, budget);
        if (loaded.error != storage::BlockJournalError::kNone || loaded.recovery_required ||
            !AuthorityMatches())
            return std::nullopt;
        return std::move(loaded.blocks);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::vector<chain::BlockIndex>> DurableSession::ActiveBlocks() const noexcept
{
    if (!View().has_value())
        return std::nullopt;
    return recovered_->chain().GetActiveBlockIndexes();
}

DurableBlockLookup DurableSession::FindBlock(const crypto::Hash256& hash) const noexcept
{
    if (!View().has_value())
        return {};
    auto block = recovered_->chain().GetBlockIndex(hash);
    if (!block && recovered_->chain().HasBlockIndex(hash))
        return {};
    return {true, std::move(block)};
}

DurableMempoolLookup DurableSession::FindMempoolEntry(const crypto::Hash256& hash) const noexcept
{
    if (!View().has_value())
        return {};
    auto entry = mempool_->GetEntry(hash);
    if (!entry && mempool_->Contains(hash))
        return {};
    return {true, std::move(entry)};
}

chain::CoinReadResult DurableSession::ReadCoin(const chain::UTXOKey& key) const noexcept
{
    if (!View().has_value())
        return {chain::CoinReadError::kUnavailable, std::nullopt};
    try {
        return recovered_->utxos().ReadCoin(key);
    } catch (...) {
        return {chain::CoinReadError::kUnavailable, std::nullopt};
    }
}

std::optional<DurableWalletView> DurableSession::WalletView() const noexcept
{
    if (!View().has_value())
        return std::nullopt;
    try {
        const auto& wallet = recovered_->wallet();
        return DurableWalletView{wallet.ConfirmedBalance(), wallet.UnconfirmedBalance(),
                                 wallet.ListUTXOs()};
    } catch (...) {
        return std::nullopt;
    }
}

SyncResult DurableSession::StartHeaderSync() noexcept
{
    if (!View().has_value())
        return {SyncError::kChainStateUnavailable, {}, {}};
    try {
        if (!synchronizer_) {
            const auto validation = chain::NetworkValidationParams(*network_);
            synchronizer_ = std::make_unique<Synchronizer>(
                SyncParams{validation, network_->difficulty, network_->protocol_version, 2U, 2'000U,
                           4'096U},
                recovered_->chain(), nullptr, nullptr);
        }
        return synchronizer_->Start();
    } catch (...) {
        return {SyncError::kAllocationFailure, {}, {}};
    }
}

SyncResult DurableSession::HandleSyncHeaders(const net::FramedMessage& message) noexcept
{
    if (!View().has_value())
        return {SyncError::kChainStateUnavailable, {}, {}};
    if (message.command != net::Command::kHeaders)
        return {SyncError::kUnexpectedMessage, {}, {}};
    if (!synchronizer_) {
        const auto start = StartHeaderSync();
        if (start.error != SyncError::kNone)
            return start;
    }
    return synchronizer_->Handle(message);
}

std::optional<DurableBodyState>
DurableSession::HeaderBodyState(const crypto::Hash256& hash) const noexcept
{
    if (!View().has_value())
        return std::nullopt;
    if (recovered_->chain().GetHeaderMetadata(hash).has_value())
        return DurableBodyState::kValidated;
    if (synchronizer_ && synchronizer_->MissingBody(hash))
        return DurableBodyState::kPending;
    return DurableBodyState::kStale;
}

std::optional<net::HeadersMessage>
DurableSession::HeadersForLocator(const net::GetHeadersMessage& request) const noexcept
{
    if (!View().has_value() || request.locator_hashes.size() > 101U)
        return std::nullopt;
    try {
        const auto active = recovered_->chain().GetActiveBlockIndexes();
        if (!active)
            return std::nullopt;
        std::size_t start{};
        for (const auto& locator : request.locator_hashes) {
            const auto found =
                std::find_if(active->begin(), active->end(),
                             [&locator](const auto& entry) { return entry.hash == locator; });
            if (found != active->end()) {
                start = static_cast<std::size_t>(std::distance(active->begin(), found)) + 1U;
                break;
            }
        }
        net::HeadersMessage response;
        for (std::size_t i = start; i < active->size() && response.headers.size() < 2'000U; ++i) {
            const auto& entry = active->at(i);
            if (!entry.block)
                continue;
            response.headers.push_back(entry.block->header);
            if (entry.hash == request.stop_hash)
                break;
        }
        return response;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<wallet::TransactionBuildResult>
DurableSession::BuildTransaction(const std::span<const wallet::Recipient> recipients) const noexcept
{
    if (!View().has_value())
        return std::nullopt;
    return recovered_->wallet().CreateTransaction(recipients);
}

std::optional<wallet::TransactionSignResult>
DurableSession::SignTransaction(const primitives::Transaction& transaction) const noexcept
{
    if (!View().has_value())
        return std::nullopt;
    return recovered_->wallet().SignTransaction(transaction);
}

std::optional<bool>
DurableSession::VerifyTransaction(const primitives::Transaction& transaction) const noexcept
{
    if (!View().has_value())
        return std::nullopt;
    return recovered_->wallet().VerifySignaturesLocally(transaction);
}

bool DurableSession::AuthorityMatches() const noexcept
{
    const auto authority = store_->Observe();
    const auto& expected = recovered_->snapshot();
    return authority.state == storage::GenerationState::kPresent &&
           authority.snapshot.sequence == expected.sequence &&
           authority.snapshot.journal_size == expected.journal_size &&
           authority.snapshot.wallet_size == expected.wallet_size &&
           authority.snapshot.journal_hash == expected.journal_hash &&
           authority.snapshot.wallet_hash == expected.wallet_hash &&
           authority.snapshot.mempool.has_value() == expected.mempool.has_value() &&
           authority.snapshot.mempool_size == expected.mempool_size &&
           authority.snapshot.mempool_hash == expected.mempool_hash;
}

DurableAddressResult
DurableSession::GenerateAddress(const std::filesystem::path& fresh_staging_directory,
                                const std::span<const std::uint8_t> passphrase) noexcept
{
    if (quarantined_ || !recovered_->StillMatchesBase(recovered_->next_time()) ||
        !AuthorityMatches()) {
        quarantined_ = true;
        return {};
    }
    auto& wallet = recovered_->wallet();
    if (passphrase.empty() ||
        !wallet.VerifyEncryptedImage(recovered_->snapshot().wallet, passphrase))
        return {DurableSessionError::kWallet, std::nullopt};
    auto address = wallet.PrepareReceivingAddress();
    if (address == nullptr)
        return {DurableSessionError::kWallet, std::nullopt};
    try {
        std::error_code fs_error;
        if (fresh_staging_directory.empty() ||
            !std::filesystem::create_directory(fresh_staging_directory, fs_error) || fs_error)
            return {DurableSessionError::kStorage, std::nullopt};
        const auto wallet_path = fresh_staging_directory / "wallet.dat";
        if (!wallet.SavePreparedAddressEncrypted(*address, wallet_path, passphrase))
            return {DurableSessionError::kWallet, std::nullopt};
        if (!recovered_->StillMatchesBase(recovered_->next_time()) || !AuthorityMatches() ||
            !wallet.CanCommitPreparedAddress(*address)) {
            quarantined_ = true;
            return {};
        }
        storage::GenerationError error{};
        const auto& base = recovered_->snapshot();
        auto staged = base.mempool.has_value()
                          ? store_->Prepare(base.journal, wallet_path, *base.mempool, error)
                          : store_->Prepare(base.journal, wallet_path, error);
        if (staged == nullptr) {
            if (!AuthorityMatches()) {
                quarantined_ = true;
                return {};
            }
            return {DurableSessionError::kStorage, std::nullopt};
        }
        const auto image = store_->InspectPrepared(*staged);
        if (image.state != storage::GenerationState::kPresent ||
            image.snapshot.journal_size != recovered_->snapshot().journal_size ||
            image.snapshot.journal_hash != recovered_->snapshot().journal_hash ||
            image.snapshot.mempool_size != recovered_->snapshot().mempool_size ||
            image.snapshot.mempool_hash != recovered_->snapshot().mempool_hash ||
            image.snapshot.mempool.has_value() != recovered_->snapshot().mempool.has_value() ||
            !recovered_->StillMatchesBase(recovered_->next_time()) || !AuthorityMatches() ||
            !wallet.CanCommitPreparedAddress(*address)) {
            quarantined_ = true;
            return {};
        }
        const auto active = recovered_->chain().GetActiveBlockIndexes();
        std::vector<primitives::Transaction> pending;
        if (image.snapshot.mempool.has_value()) {
            std::ifstream input{*image.snapshot.mempool, std::ios::binary};
            const auto bytes = chain::ReadMempoolImage(input, mempool_->parameters());
            const auto records =
                bytes ? chain::DecodeMempoolImage(*bytes, mempool_->parameters()) : std::nullopt;
            if (!records) {
                quarantined_ = true;
                return {};
            }
            for (const auto& record : *records)
                pending.push_back(record.transaction);
        }
        const wallet::WalletParams wallet_params{network_->block_limits.transaction_limits, 1, 1U,
                                                 true};
        auto staged_wallet =
            wallet::Wallet::LoadEncrypted(wallet_params, image.snapshot.wallet, passphrase);
        if (!active || !staged_wallet ||
            staged_wallet->NextReceivingAddressIndex() != address->address().index + 1U ||
            !staged_wallet->MatchesFullReplay(*active, pending,
                                              network_->id == consensus::NetworkId::kRegtest)) {
            quarantined_ = true;
            return {};
        }
        auto expected_addresses = wallet.ListReceivingAddresses();
        expected_addresses.push_back(address->address());
        const auto actual_addresses = staged_wallet->ListReceivingAddresses();
        if (expected_addresses.size() != actual_addresses.size()) {
            quarantined_ = true;
            return {};
        }
        for (const auto& expected : expected_addresses) {
            const auto found = std::find_if(
                actual_addresses.begin(), actual_addresses.end(), [&](const auto& actual) {
                    return actual.index == expected.index &&
                           actual.public_key_hash == expected.public_key_hash &&
                           actual.public_key.SerializeCompressed() ==
                               expected.public_key.SerializeCompressed();
                });
            if (found == actual_addresses.end()) {
                quarantined_ = true;
                return {};
            }
        }
        auto durable = store_->Commit(*staged);
        if (durable.state == storage::GenerationCommitState::kUnavailable)
            durable = store_->Resolve(*staged);
        if (durable.state == storage::GenerationCommitState::kNotCommitted)
            return {DurableSessionError::kStorage, std::nullopt};
        if (durable.state != storage::GenerationCommitState::kCommitted) {
            quarantined_ = true;
            recovered_->Invalidate();
            return {};
        }
        const auto public_address = address->address();
        if (!wallet.CommitPreparedAddress(*address) ||
            !recovered_->AdvanceAfterPublication(*store_, recovered_->next_time())) {
            quarantined_ = true;
            recovered_->Invalidate();
            return {};
        }
        return {DurableSessionError::kNone, public_address};
    } catch (...) {
        quarantined_ = true;
        recovered_->Invalidate();
        return {};
    }
}

DurableSessionError
DurableSession::AdmitTransaction(const primitives::Transaction& transaction,
                                 const std::filesystem::path& directory,
                                 const std::span<const std::uint8_t> passphrase) noexcept
{
    if (quarantined_ || !recovered_->StillMatchesBase(recovered_->next_time()) ||
        !AuthorityMatches()) {
        quarantined_ = true;
        return DurableSessionError::kUnavailable;
    }
    auto& wallet = recovered_->wallet();
    if (passphrase.empty() ||
        !wallet.VerifyEncryptedImage(recovered_->snapshot().wallet, passphrase))
        return DurableSessionError::kWallet;
    try {
        if (recovered_->chain().active_height() == std::numeric_limits<std::uint32_t>::max())
            return DurableSessionError::kContext;
        auto view = recovered_->utxos().BeginBatch();
        if (!view)
            return DurableSessionError::kContext;
        auto admission = mempool_->PrepareAdmission(
            transaction, {recovered_->chain().active_height() + 1U, recovered_->next_time()}, *view,
            recovered_->next_time());
        if (!admission.prepared)
            return DurableSessionError::kMempool;
        auto observed = wallet.PrepareObservedState(transaction, 0U, false);
        const auto bytes = chain::EncodePreparedMempoolImage(*mempool_, *admission.prepared,
                                                             mempool_->parameters());
        const auto records =
            bytes ? chain::DecodeMempoolImage(*bytes, mempool_->parameters()) : std::nullopt;
        const auto active = recovered_->chain().GetActiveBlockIndexes();
        if (!observed || !bytes || !records || !active)
            return DurableSessionError::kContext;
        std::vector<primitives::Transaction> pending;
        for (const auto& record : *records)
            pending.push_back(record.transaction);
        if (!wallet.MatchesPreparedReplay(*observed, *active, pending,
                                          network_->id == consensus::NetworkId::kRegtest))
            return DurableSessionError::kWallet;
        std::error_code fs_error;
        if (directory.empty() || !std::filesystem::create_directory(directory, fs_error) ||
            fs_error)
            return DurableSessionError::kStorage;
        const auto wallet_path = directory / "wallet.dat";
        const auto mempool_path = directory / "mempool.dat";
        if (!wallet.SavePreparedEncrypted(*observed, wallet_path, passphrase))
            return DurableSessionError::kWallet;
        std::ofstream output{mempool_path, std::ios::binary};
        output.write(reinterpret_cast<const char*>(bytes->data()),
                     static_cast<std::streamsize>(bytes->size()));
        output.close();
        if (!output)
            return DurableSessionError::kStorage;
        if (!view->IsCurrent() || !recovered_->StillMatchesBase(recovered_->next_time()) ||
            !AuthorityMatches() || !wallet.CanCommitPrepared(*observed) ||
            !mempool_->CanCommitPrepared(*admission.prepared)) {
            quarantined_ = true;
            return DurableSessionError::kUnavailable;
        }
        storage::GenerationError error{};
        auto staged =
            store_->Prepare(recovered_->snapshot().journal, wallet_path, mempool_path, error);
        if (!staged) {
            if (!AuthorityMatches()) {
                quarantined_ = true;
                return DurableSessionError::kUnavailable;
            }
            return DurableSessionError::kStorage;
        }
        const auto image = store_->InspectPrepared(*staged);
        const auto digest = crypto::Hash256::Sha256(*bytes);
        if (!digest || image.state != storage::GenerationState::kPresent ||
            image.snapshot.journal_hash != recovered_->snapshot().journal_hash ||
            image.snapshot.journal_size != recovered_->snapshot().journal_size ||
            image.snapshot.mempool_size != bytes->size() ||
            image.snapshot.mempool_hash != *digest) {
            quarantined_ = true;
            return DurableSessionError::kUnavailable;
        }
        const wallet::WalletParams wallet_params{network_->block_limits.transaction_limits, 1, 1U,
                                                 true};
        auto staged_wallet =
            wallet::Wallet::LoadEncrypted(wallet_params, image.snapshot.wallet, passphrase);
        if (!staged_wallet ||
            staged_wallet->NextReceivingAddressIndex() != wallet.NextReceivingAddressIndex() ||
            !staged_wallet->MatchesFullReplay(*active, pending,
                                              network_->id == consensus::NetworkId::kRegtest)) {
            quarantined_ = true;
            return DurableSessionError::kUnavailable;
        }
        const auto expected_addresses = wallet.ListReceivingAddresses();
        const auto staged_addresses = staged_wallet->ListReceivingAddresses();
        if (expected_addresses.size() != staged_addresses.size()) {
            quarantined_ = true;
            return DurableSessionError::kUnavailable;
        }
        for (std::size_t i = 0; i < expected_addresses.size(); ++i) {
            if (expected_addresses[i].index != staged_addresses[i].index ||
                expected_addresses[i].public_key_hash != staged_addresses[i].public_key_hash ||
                expected_addresses[i].public_key.SerializeCompressed() !=
                    staged_addresses[i].public_key.SerializeCompressed()) {
                quarantined_ = true;
                return DurableSessionError::kUnavailable;
            }
        }
        if (!view->IsCurrent() || !recovered_->StillMatchesBase(recovered_->next_time()) ||
            !AuthorityMatches() || !wallet.CanCommitPrepared(*observed) ||
            !mempool_->CanCommitPrepared(*admission.prepared)) {
            quarantined_ = true;
            return DurableSessionError::kUnavailable;
        }
        auto durable = store_->Commit(*staged);
        if (durable.state == storage::GenerationCommitState::kUnavailable)
            durable = store_->Resolve(*staged);
        if (durable.state == storage::GenerationCommitState::kNotCommitted)
            return DurableSessionError::kStorage;
        if (durable.state != storage::GenerationCommitState::kCommitted ||
            !wallet.CommitPreparedState(*observed) ||
            !mempool_->CommitPrepared(*admission.prepared) ||
            !recovered_->AdvanceAfterPublication(*store_, recovered_->next_time())) {
            quarantined_ = true;
            recovered_->Invalidate();
            return DurableSessionError::kUnavailable;
        }
        return DurableSessionError::kNone;
    } catch (...) {
        quarantined_ = true;
        recovered_->Invalidate();
        return DurableSessionError::kUnavailable;
    }
}

DurableTransactionResult
DurableSession::BuildAndAdmit(const std::span<const wallet::Recipient> recipients,
                              const std::filesystem::path& directory,
                              const std::span<const std::uint8_t> passphrase) noexcept
{
    auto built = BuildTransaction(recipients);
    if (!built)
        return {DurableSessionError::kUnavailable, std::nullopt};
    if (!built->built)
        return {DurableSessionError::kWallet, std::nullopt};
    auto transaction = std::move(built->built->transaction);
    const auto result = AdmitTransaction(transaction, directory, passphrase);
    if (result == DurableSessionError::kUnavailable)
        return {result, std::move(transaction)};
    if (result != DurableSessionError::kNone)
        return {result, std::nullopt};
    return {DurableSessionError::kNone, std::move(transaction)};
}

DurableSessionResult
DurableSession::ApplyBlock(const primitives::Block& block,
                           const std::span<const primitives::Transaction> known,
                           const std::filesystem::path& fresh_staging_directory,
                           const std::span<const std::uint8_t> passphrase) noexcept
{
    return ApplyBlockInternal(block, known, fresh_staging_directory, passphrase, nullptr);
}

DurableSessionResult
DurableSession::ApplyBlockInternal(const primitives::Block& block,
                                   const std::span<const primitives::Transaction> known,
                                   const std::filesystem::path& fresh_staging_directory,
                                   const std::span<const std::uint8_t> passphrase,
                                   wallet::Wallet::PreparedAddress* address) noexcept
{
    if (quarantined_ || !recovered_->StillMatchesBase(recovered_->next_time()) ||
        !AuthorityMatches()) {
        quarantined_ = true;
        return {};
    }
    auto prepared = PrepareNodeBlock(recovered_->chain(), recovered_->wallet(), *mempool_, block,
                                     known, recovered_->next_time(), address);
    if (prepared.candidate == nullptr) {
        const auto error =
            prepared.error == BlockPreparationError::kChain     ? DurableSessionError::kChain
            : prepared.error == BlockPreparationError::kWallet  ? DurableSessionError::kWallet
            : prepared.error == BlockPreparationError::kMempool ? DurableSessionError::kMempool
                                                                : DurableSessionError::kContext;
        return {error, prepared.chain_result};
    }
    GenerationStageError stage_error{};
    auto stage = prepared.candidate->StageRecoveredGeneration(
        *store_, *recovered_, *mempool_, recovered_->next_time(), fresh_staging_directory,
        passphrase, stage_error, address);
    if (stage == nullptr) {
        std::fprintf(stderr, "durable block staging failed: stage_error=%u\n",
                     static_cast<unsigned>(stage_error));
        if (!AuthorityMatches() || stage_error == GenerationStageError::kStale ||
            stage_error == GenerationStageError::kInvalidBase) {
            quarantined_ = true;
            return {DurableSessionError::kUnavailable, prepared.chain_result};
        }
        return {stage_error == GenerationStageError::kWallet ? DurableSessionError::kWallet
                                                             : DurableSessionError::kStorage,
                prepared.chain_result};
    }
    const auto committed =
        prepared.candidate->CommitGeneration(*stage, *store_, *recovered_, *mempool_, address);
    if (committed == NodePublicationResult::kCommitted)
        return {DurableSessionError::kNone, prepared.chain_result};
    if (committed == NodePublicationResult::kNotCommitted)
        return {DurableSessionError::kStorage, prepared.chain_result};
    std::fprintf(stderr, "durable block publication unavailable\n");
    quarantined_ = true;
    return {DurableSessionError::kUnavailable, prepared.chain_result};
}

DurableSessionResult
DurableSession::MineBlock(const std::span<const primitives::Transaction> known,
                          const std::filesystem::path& fresh_staging_directory,
                          const std::span<const std::uint8_t> passphrase) noexcept
{
    if (quarantined_ || !recovered_->StillMatchesBase(recovered_->next_time()) ||
        !AuthorityMatches()) {
        quarantined_ = true;
        return {};
    }
    if (network_->id != consensus::NetworkId::kRegtest ||
        recovered_->chain().active_height() == std::numeric_limits<std::uint32_t>::max())
        return {DurableSessionError::kMining, {}};
    try {
        const auto addresses = recovered_->wallet().ListReceivingAddresses();
        std::unique_ptr<wallet::Wallet::PreparedAddress> pending;
        if (addresses.empty())
            pending = recovered_->wallet().PrepareReceivingAddress();
        if (addresses.empty() && pending == nullptr)
            return {DurableSessionError::kWallet, {}};
        const auto& address = addresses.empty() ? pending->address() : addresses.front();
        const auto selected = mempool_->SelectForBlock(network_->block_limits.max_serialized_size);
        if (selected.error != chain::MempoolError::kNone)
            return {DurableSessionError::kMempool, {}};
        primitives::Amount fees{};
        for (const auto& transaction : selected.transactions) {
            const auto txid = transaction.TxId(network_->block_limits.transaction_limits);
            const auto entry = txid.has_value() ? mempool_->GetEntry(*txid) : std::nullopt;
            if (!entry.has_value() || entry->fee < 0 ||
                entry->fee > network_->monetary.max_money - fees)
                return {DurableSessionError::kMempool, {}};
            fees += entry->fee;
        }
        const auto height = recovered_->chain().active_height() + 1U;
        const auto subsidy = consensus::GetBlockSubsidy(height, network_->monetary);
        if (!subsidy.has_value() || fees > network_->monetary.max_money - *subsidy)
            return {DurableSessionError::kMining, {}};
        primitives::Transaction coinbase{
            1,
            {{primitives::OutPoint{crypto::Hash256{}, std::numeric_limits<std::uint32_t>::max()},
              CoinbaseHeightScript(height), 0U}},
            {{*subsidy + fees, P2pkhScript(address.public_key_hash)}},
            0U};
        primitives::Block block{{1, recovered_->chain().active_tip(), crypto::Hash256{},
                                 recovered_->next_time(), network_->pow.pow_limit_compact, 0U},
                                {std::move(coinbase)}};
        block.transactions.insert(block.transactions.end(), selected.transactions.begin(),
                                  selected.transactions.end());
        const auto root = primitives::ComputeMerkleRoot(block.transactions,
                                                        network_->block_limits.transaction_limits);
        if (!root.has_value())
            return {DurableSessionError::kMining, {}};
        block.header.merkle_root = *root;
        const auto mined = consensus::MineRegtestBlock(block.header, 4'096U);
        if (!mined.has_value())
            return {DurableSessionError::kMining, {}};
        block.header = mined->header;
        return ApplyBlockInternal(block, known, fresh_staging_directory, passphrase, pending.get());
    } catch (...) {
        return {DurableSessionError::kMining, {}};
    }
}

void DurableSession::FailStoreForTesting(const storage::GenerationFailurePoint point) noexcept
{
    store_->FailNextForTesting(point);
}
void DurableSession::SetStoreIoFaultForTesting(const storage::GenerationIoTarget target,
                                               const storage::GenerationIoFault fault) noexcept
{
    store_->SetIoFaultForTesting(target, fault);
}
bool DurableSession::StoreIoFaultTriggeredForTesting() const noexcept
{
    return store_->IoFaultTriggeredForTesting();
}
void DurableSession::FailAdvanceForTesting() noexcept
{
    recovered_->FailNextAdvanceForTesting();
}
} // namespace nova::node
