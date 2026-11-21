#include "node/regtest_node.hpp"
#include "node/data_directory_lease.hpp"

#include "chain/network_chainstate.hpp"
#include "consensus/monetary.hpp"
#include "node/mining_helpers.hpp"
#include "node/network_selection.hpp"
#include "storage/network_identity.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <new>
#include <system_error>
#include <utility>

namespace nova::node
{
namespace
{

[[nodiscard]] std::vector<std::uint8_t> P2pkhScript(const crypto::Hash160& hash)
{
    std::vector<std::uint8_t> script{0x76U, 0xA9U, 0x14U};
    script.insert(script.end(), hash.begin(), hash.end());
    script.insert(script.end(), {0x88U, 0xACU});
    return script;
}

[[nodiscard]] std::uint64_t LocalWallClockSeconds() noexcept
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return seconds <= 0 ? 0U : static_cast<std::uint64_t>(seconds);
}

} // namespace

RegtestNode::RegtestNode(std::unique_ptr<storage::GenerationStore> directory_lease,
                         RegtestNodeConfig config, storage::BlockJournal journal,
                         std::unique_ptr<chain::UTXOSet> utxos,
                         std::unique_ptr<chain::ChainState> chain,
                         std::unique_ptr<chain::Mempool> mempool,
                         std::unique_ptr<wallet::Wallet> wallet,
                         std::shared_ptr<const chain::ValidationTimeSource> validation_time_source,
                         const consensus::NetworkParams& network) noexcept
    : directory_lease_(std::move(directory_lease)), config_(std::move(config)),
      journal_(std::move(journal)), utxos_(std::move(utxos)), chain_(std::move(chain)),
      mempool_(std::move(mempool)), wallet_(std::move(wallet)),
      validation_time_source_(std::move(validation_time_source)), network_(&network),
      next_time_(config_.start_time), started_at_(std::chrono::steady_clock::now())
{
}

std::unique_ptr<RegtestNode> RegtestNode::Create(RegtestNodeConfig config) noexcept
{
    const auto selection = SelectNetwork(config.network);
    if (selection.error != NetworkSelectionError::kNone || selection.parameters == nullptr ||
        config.name.empty() || config.data_directory.empty() || config.log_path.empty() ||
        config.p2p_port == 0U || config.rpc_port == 0U || config.p2p_port == config.rpc_port ||
        config.start_time < selection.parameters->genesis_block.header.time) {
        return nullptr;
    }
    const auto& network = *selection.parameters;
    try {
        auto directory_lease = AcquireDataDirectoryLease(config.data_directory, network);
        if (!directory_lease)
            return nullptr;
        std::error_code identity_error;
        const auto identity_status = std::filesystem::symlink_status(
            storage::NetworkIdentityMarkerPath(config.data_directory), identity_error);
        if (identity_error && identity_error != std::errc::no_such_file_or_directory)
            return nullptr;
        const bool identity_existed = !identity_error && std::filesystem::exists(identity_status);
        if (storage::EnsureNetworkIdentityWithLease(config.data_directory, network) !=
            storage::NetworkIdentityError::kNone) {
            return nullptr;
        }
        // Never create a separate legacy wallet and journal beside an existing
        // durable store. The storage modes do not share publication authority.
        std::error_code generation_error;
        const auto generation_status = std::filesystem::symlink_status(
            config.data_directory / "generations", generation_error);
        if ((generation_error && generation_error != std::errc::no_such_file_or_directory) ||
            (!generation_error && std::filesystem::exists(generation_status))) {
            return nullptr;
        }
        const auto wallet_path = config.data_directory / "wallet.dat";
        const auto journal_path = config.data_directory / "blocks.dat";
        const auto plain_file_state = [](const std::filesystem::path& path) -> std::optional<bool> {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error == std::errc::no_such_file_or_directory)
                return false;
            if (error)
                return std::nullopt;
            if (!std::filesystem::exists(status))
                return false;
            if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status))
                return std::nullopt;
            return true;
        };
        const auto wallet_exists = plain_file_state(wallet_path);
        const auto journal_exists = plain_file_state(journal_path);
        if (!wallet_exists || !journal_exists ||
            (*wallet_exists && config.wallet_passphrase.empty()) ||
            (!config.wallet_passphrase.empty() && identity_existed && !*wallet_exists &&
             !*journal_exists) ||
            (!config.wallet_passphrase.empty() && *wallet_exists != *journal_exists)) {
            return nullptr;
        }
        auto journal = storage::BlockJournal::Open(config.data_directory, network.block_limits);
        if (journal && !*journal_exists) {
            // Persist an explicit empty journal for a fresh legacy directory.
            // A later missing journal is then distinguishable from a new node.
            std::ofstream empty_journal{journal_path, std::ios::binary | std::ios::app};
            empty_journal.close();
            if (!empty_journal)
                return nullptr;
        }
        const wallet::WalletParams wallet_parameters{network.block_limits.transaction_limits, 1, 1U,
                                                     true};
        auto wallet = *wallet_exists ? wallet::Wallet::LoadEncrypted(wallet_parameters, wallet_path,
                                                                     config.wallet_passphrase)
                                     : wallet::Wallet::Create(wallet_parameters);
        auto mempool = chain::Mempool::Create(
            chain::MempoolParams{chain::NetworkValidationParams(network), 1'000U, 10'000'000U, 0});
        if (!journal.has_value() || wallet == nullptr || mempool == nullptr) {
            return nullptr;
        }
        auto validation_time_source = std::make_shared<chain::SystemValidationTimeSource>();
        auto initialized =
            chain::NetworkChainInitializer::Create(config.network, validation_time_source);
        if (!initialized.has_value()) {
            return nullptr;
        }
        std::error_code filesystem_error;
        std::filesystem::create_directories(config.log_path.parent_path(), filesystem_error);
        if (filesystem_error) {
            return nullptr;
        }
        std::ofstream log{config.log_path, std::ios::app};
        if (!log.is_open()) {
            return nullptr;
        }
        auto node = std::unique_ptr<RegtestNode>{new RegtestNode{
            std::move(directory_lease), std::move(config), std::move(*journal),
            std::move(initialized->utxos), std::move(initialized->chain), std::move(mempool),
            std::move(wallet), std::move(validation_time_source), network}};
        const auto saved = node->journal_.Load();
        if (saved.error != storage::BlockJournalError::kNone) {
            return nullptr;
        }
        if (*wallet_exists) {
            // Validate the stored pair against a private replay before the live
            // wallet can be rebuilt or saved. Legacy mempool entries are not
            // persisted, so pending wallet state cannot be reconstructed here.
            auto verified =
                chain::NetworkChainInitializer::Create(network.id, node->validation_time_source_);
            if (!verified)
                return nullptr;
            for (const auto& block : saved.blocks) {
                if (block.header.time == std::numeric_limits<std::uint32_t>::max() ||
                    verified->chain->AcceptBlock(block).error != chain::ChainError::kNone)
                    return nullptr;
            }
            const auto active = verified->chain->GetActiveBlockIndexes();
            if (!active || !node->wallet_->MatchesFullReplay(
                               *active, {}, network.id == consensus::NetworkId::kRegtest)) {
                return nullptr;
            }
        }
        if (network.id == consensus::NetworkId::kTestnet) {
            const auto active = node->chain_->GetActiveBlockIndexes();
            if (!active.has_value() || !node->wallet_->RebuildConfirmedState(*active)) {
                return nullptr;
            }
        }
        for (const auto& block : saved.blocks) {
            if (node->AcceptBlock(block, false) != RegtestNodeError::kNone) {
                return nullptr;
            }
        }
        if (!node->PersistWallet()) {
            return nullptr;
        }
        node->Log("started");
        return node;
    } catch (...) {
        return nullptr;
    }
}

bool RegtestNode::Connect(RegtestNode& peer) noexcept
{
    if (&peer == this || std::any_of(peers_.begin(), peers_.end(), [&peer](const auto& existing) {
            return &existing.get() == &peer;
        })) {
        return false;
    }
    try {
        peers_.push_back(peer);
        peer.peers_.push_back(*this);
        const auto local_blocks = journal_.Load();
        const auto remote_blocks = peer.journal_.Load();
        if (local_blocks.error != storage::BlockJournalError::kNone ||
            remote_blocks.error != storage::BlockJournalError::kNone) {
            DisconnectAll();
            return false;
        }
        for (const auto& block : local_blocks.blocks) {
            const auto result = peer.ReceiveBlock(block);
            if (result != RegtestNodeError::kNone && result != RegtestNodeError::kDuplicateBlock) {
                DisconnectAll();
                return false;
            }
        }
        for (const auto& block : remote_blocks.blocks) {
            const auto result = ReceiveBlock(block);
            if (result != RegtestNodeError::kNone && result != RegtestNodeError::kDuplicateBlock) {
                DisconnectAll();
                return false;
            }
        }
        Log("peer connected");
        peer.Log("peer connected");
        return true;
    } catch (...) {
        return false;
    }
}

void RegtestNode::DisconnectAll() noexcept
{
    for (auto& peer : peers_) {
        auto& links = peer.get().peers_;
        links.erase(std::remove_if(links.begin(), links.end(),
                                   [this](const auto& item) { return &item.get() == this; }),
                    links.end());
    }
    peers_.clear();
    Log("peers disconnected");
}

RegtestNodeResult RegtestNode::MineBlock() noexcept
{
    const auto& network = *network_;
    if (network.id != consensus::NetworkId::kRegtest) {
        return {RegtestNodeError::kMiningFailure, std::nullopt, std::nullopt};
    }
    try {
        const auto addresses = wallet_->ListReceivingAddresses();
        const auto address = addresses.empty()
                                 ? wallet_->GenerateReceivingAddress()
                                 : std::optional<wallet::WalletAddress>{addresses.front()};
        if (!address.has_value() || height() == std::numeric_limits<std::uint32_t>::max()) {
            return {RegtestNodeError::kWalletFailure, std::nullopt, std::nullopt};
        }
        const auto selected = mempool_->SelectForBlock(network.block_limits.max_serialized_size);
        if (selected.error != chain::MempoolError::kNone) {
            return {RegtestNodeError::kMempoolFailure, std::nullopt, std::nullopt};
        }
        primitives::Amount fees{};
        for (const auto& transaction : selected.transactions) {
            const auto txid = transaction.TxId(network.block_limits.transaction_limits);
            const auto entry = txid.has_value() ? mempool_->GetEntry(*txid) : std::nullopt;
            if (!entry.has_value() || entry->fee > network.monetary.max_money - fees) {
                return {RegtestNodeError::kMempoolFailure, std::nullopt, std::nullopt};
            }
            fees += entry->fee;
        }
        const auto next_height = height() + 1U;
        const auto subsidy = consensus::GetBlockSubsidy(next_height, network.monetary);
        if (!subsidy.has_value() || fees > network.monetary.max_money - *subsidy) {
            return {RegtestNodeError::kMiningFailure, std::nullopt, std::nullopt};
        }
        primitives::Transaction coinbase{
            1,
            {{primitives::OutPoint{crypto::Hash256{}, std::numeric_limits<std::uint32_t>::max()},
              CoinbaseHeightScript(next_height), 0U}},
            {{*subsidy + fees, P2pkhScript(address->public_key_hash)}},
            0U};
        primitives::Block block{
            {1, tip(), crypto::Hash256{}, next_time_, network.pow.pow_limit_compact, 0U},
            {std::move(coinbase)}};
        block.transactions.insert(block.transactions.end(), selected.transactions.begin(),
                                  selected.transactions.end());
        const auto root = primitives::ComputeMerkleRoot(block.transactions,
                                                        network.block_limits.transaction_limits);
        if (!root.has_value()) {
            return {RegtestNodeError::kMiningFailure, std::nullopt, std::nullopt};
        }
        block.header.merkle_root = *root;
        const auto mined = consensus::MineRegtestBlock(block.header, 4'096U);
        if (!mined.has_value()) {
            return {RegtestNodeError::kMiningFailure, std::nullopt, std::nullopt};
        }
        block.header = mined->header;
        if (ReceiveBlock(block) != RegtestNodeError::kNone) {
            return {RegtestNodeError::kBlockRejected, std::nullopt, std::nullopt};
        }
        RelayBlock(block);
        return {RegtestNodeError::kNone, mined->block_hash, std::nullopt};
    } catch (const std::bad_alloc&) {
        return {RegtestNodeError::kAllocationFailure, std::nullopt, std::nullopt};
    } catch (...) {
        return {RegtestNodeError::kMiningFailure, std::nullopt, std::nullopt};
    }
}

RegtestNodeResult
RegtestNode::CreateAndBroadcast(const std::span<const wallet::Recipient> recipients) noexcept
{
    auto built = wallet_->CreateTransaction(recipients);
    if (!built.built.has_value()) {
        return {RegtestNodeError::kWalletFailure, std::nullopt, std::nullopt};
    }
    const auto admitted = AdmitTransaction(built.built->transaction, true);
    if (admitted != RegtestNodeError::kNone)
        return {admitted, std::nullopt, std::nullopt};
    RelayTransaction(built.built->transaction);
    return {RegtestNodeError::kNone, std::nullopt, std::move(built.built->transaction)};
}

RegtestNodeError RegtestNode::ReceiveBlock(const primitives::Block& block) noexcept
{
    const auto result = AcceptBlock(block, true);
    if (result == RegtestNodeError::kNone) {
        observability::SaturatingAdd(metrics_.blocks_accepted, 1U);
        metrics_.last_block_arrival_time_seconds = LocalWallClockSeconds();
    } else if (result != RegtestNodeError::kDuplicateBlock) {
        observability::SaturatingAdd(metrics_.blocks_rejected, 1U);
    }
    return result;
}

RegtestNodeError RegtestNode::AcceptBlock(const primitives::Block& block,
                                          const bool persist) noexcept
{
    const auto hash = primitives::ComputeBlockHash(block.header);
    if (!hash.has_value()) {
        return RegtestNodeError::kBlockRejected;
    }
    if (chain_->GetBlockIndex(*hash).has_value()) {
        return RegtestNodeError::kDuplicateBlock;
    }
    if (block.header.time == std::numeric_limits<std::uint32_t>::max()) {
        return RegtestNodeError::kBlockRejected;
    }
    if (primitives::CheckBlockStructure(block, network_->block_limits) !=
        primitives::BlockStructureError::kNone) {
        return RegtestNodeError::kBlockRejected;
    }
    const auto active_tip_before = chain_->active_tip();
    if (persist) {
        std::error_code wallet_error;
        const auto wallet_status =
            std::filesystem::symlink_status(config_.data_directory / "wallet.dat", wallet_error);
        if ((wallet_error && wallet_error != std::errc::no_such_file_or_directory) ||
            (!wallet_error && std::filesystem::exists(wallet_status) &&
             (!std::filesystem::is_regular_file(wallet_status) ||
              std::filesystem::is_symlink(wallet_status)))) {
            return RegtestNodeError::kWalletFailure;
        }
    }
    if (persist && journal_.Prepare(block) != storage::BlockJournalError::kNone) {
        return RegtestNodeError::kStorageFailure;
    }
    const auto accepted = chain_->AcceptBlock(block);
    if (accepted.error != chain::ChainError::kNone) {
        return RegtestNodeError::kBlockRejected;
    }
    const auto next_block_time = block.header.time + 1U;
    next_time_ = std::max(next_time_, next_block_time);
    // A valid inactive block may become an ancestor of the winning branch.
    // Persist every accepted block before reporting success, so replay never
    // encounters a committed descendant whose accepted parent was omitted.
    const auto persist_accepted = [this, persist, &hash, &active_tip_before]() noexcept {
        if (persist && journal_.Commit(*hash) != storage::BlockJournalError::kNone) {
            // Resolve a write that may have reached durable storage before its
            // final acknowledgement. This legacy path still requires coordinator
            // integration for joint wallet/mempool publication and full ambiguity.
            if (!journal_.IsCommitted(*hash)) {
                const auto rollback = chain_->RollbackAcceptedBlock(*hash, active_tip_before);
                if (rollback.error != chain::ChainError::kNone)
                    return RegtestNodeError::kChainInitializationFailure;
                return RegtestNodeError::kStorageFailure;
            }
        }
        return RegtestNodeError::kNone;
    };
    if (!accepted.became_active) {
        const auto persisted = persist_accepted();
        if (persisted != RegtestNodeError::kNone)
            return persisted;
        Log("inactive block accepted");
        return RegtestNodeError::kNone;
    }
    const auto active = chain_->GetActiveBlockIndexes();
    if (!active.has_value()) {
        return RegtestNodeError::kChainInitializationFailure;
    }
    const auto persisted = persist_accepted();
    if (persisted != RegtestNodeError::kNone)
        return persisted;
    if (!wallet_->RebuildConfirmedState(*active)) {
        return RegtestNodeError::kWalletFailure;
    }
    for (const auto& index : *active) {
        if (index.block.has_value()) {
            (void)mempool_->RemoveForBlock(index.block->transactions);
        }
    }
    (void)mempool_->RevalidateMempool({height() + 1U, next_time_}, *utxos_);
    for (const auto& transaction : known_transactions_) {
        (void)mempool_->AcceptToMempool(transaction, {height() + 1U, next_time_}, *utxos_,
                                        next_time_);
    }
    if (persist && !PersistWallet())
        return RegtestNodeError::kWalletFailure;
    Log("block accepted");
    return RegtestNodeError::kNone;
}

RegtestNodeError
RegtestNode::ReceiveTransaction(const primitives::Transaction& transaction) noexcept
{
    return AdmitTransaction(transaction, false);
}

RegtestNodeError RegtestNode::AdmitTransaction(const primitives::Transaction& transaction,
                                               const bool observe_wallet) noexcept
{
    const auto reject = [this](const RegtestNodeError error) {
        observability::SaturatingAdd(metrics_.transactions_rejected, 1U);
        return error;
    };
    try {
        if (height() == std::numeric_limits<std::uint32_t>::max())
            return reject(RegtestNodeError::kMempoolFailure);
        auto view = utxos_->BeginBatch();
        if (!view)
            return reject(RegtestNodeError::kMempoolFailure);
        auto admission =
            mempool_->PrepareAdmission(transaction, {height() + 1U, next_time_}, *view, next_time_);
        if (admission.result.error != chain::MempoolError::kNone || !admission.prepared)
            return reject(RegtestNodeError::kMempoolFailure);
        // All allocation and wallet observation precede publication. In
        // particular a failed save must not leave a spend in the live pool.
        auto known = known_transactions_;
        known.push_back(transaction);
        auto observed =
            observe_wallet ? wallet_->PrepareObservedState(transaction, 0U, false) : nullptr;
        if (observe_wallet && !observed)
            return reject(RegtestNodeError::kWalletFailure);
        if (!view->IsCurrent() || !mempool_->CanCommitPrepared(*admission.prepared) ||
            (observed && !wallet_->CanCommitPrepared(*observed)))
            return reject(RegtestNodeError::kMempoolFailure);
        const bool saved =
            observed
                ? (config_.wallet_passphrase.empty() ||
                   wallet_->SavePreparedEncrypted(*observed, config_.data_directory / "wallet.dat",
                                                  config_.wallet_passphrase))
                : PersistWallet();
        if (!saved)
            return reject(RegtestNodeError::kWalletFailure);
        // Single-threaded, non-reentrant ownership is required. These commits
        // publish prepared state without allocations. This legacy path still
        // does not persist the mempool jointly; it is not crash-atomic recovery.
        if (!view->IsCurrent() || !mempool_->CanCommitPrepared(*admission.prepared) ||
            (observed && !wallet_->CanCommitPrepared(*observed)))
            return reject(RegtestNodeError::kMempoolFailure);
        if (observed && !wallet_->CommitPreparedState(*observed))
            return reject(RegtestNodeError::kWalletFailure);
        if (!mempool_->CommitPrepared(*admission.prepared))
            return reject(RegtestNodeError::kMempoolFailure);
        known_transactions_.swap(known);
        observability::SaturatingAdd(metrics_.transactions_accepted, 1U);
        return RegtestNodeError::kNone;
    } catch (...) {
        return reject(RegtestNodeError::kAllocationFailure);
    }
}

RegtestNodeError RegtestNode::SubmitTransaction(const primitives::Transaction& transaction) noexcept
{
    const auto result = ReceiveTransaction(transaction);
    if (result == RegtestNodeError::kNone) {
        RelayTransaction(transaction);
    }
    return result;
}

std::uint32_t RegtestNode::height() const noexcept
{
    return chain_->active_height();
}
std::uint32_t RegtestNode::next_mining_time() const noexcept
{
    return next_time_;
}
const crypto::Hash256& RegtestNode::tip() const noexcept
{
    return chain_->active_tip();
}
std::size_t RegtestNode::mempool_size() const noexcept
{
    return mempool_->size();
}
const chain::ChainState& RegtestNode::chain_state() const noexcept
{
    return *chain_;
}
chain::ChainState& RegtestNode::chain_state() noexcept
{
    return *chain_;
}
chain::UTXOSet& RegtestNode::utxos() noexcept
{
    return *utxos_;
}
chain::Mempool& RegtestNode::mempool() noexcept
{
    return *mempool_;
}
wallet::Wallet& RegtestNode::wallet() noexcept
{
    return *wallet_;
}
const wallet::Wallet& RegtestNode::wallet() const noexcept
{
    return *wallet_;
}
std::optional<wallet::WalletAddress> RegtestNode::GenerateReceivingAddress() noexcept
{
    auto prepared = wallet_->PrepareReceivingAddress();
    if (!prepared || !wallet_->CanCommitPreparedAddress(*prepared))
        return std::nullopt;
    // Save the private candidate, not a mutation of the live wallet. With the
    // node exclusively held, the prepared key remains current through commit.
    if (!config_.wallet_passphrase.empty() &&
        !wallet_->SavePreparedAddressEncrypted(*prepared, config_.data_directory / "wallet.dat",
                                               config_.wallet_passphrase))
        return std::nullopt;
    const auto address = prepared->address();
    if (!wallet_->CommitPreparedAddress(*prepared))
        return std::nullopt;
    return address;
}

bool RegtestNode::PersistWalletForRpc() const noexcept
{
    return PersistWallet();
}
bool RegtestNode::PersistWallet() const noexcept
{
    // Empty passphrases intentionally mean the caller requested ephemeral
    // regtest state; no marker or plaintext file is ever written.
    return config_.wallet_passphrase.empty() ||
           wallet_->SaveEncrypted(config_.data_directory / "wallet.dat", config_.wallet_passphrase);
}
const RegtestNodeConfig& RegtestNode::config() const noexcept
{
    return config_;
}
const consensus::NetworkParams& RegtestNode::network_params() const noexcept
{
    return *network_;
}
observability::MetricsSnapshot RegtestNode::metrics() const noexcept
{
    auto result = metrics_;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started_at_);
    result.uptime_seconds = elapsed.count() <= 0 ? 0U : static_cast<std::uint64_t>(elapsed.count());
    return result;
}
void RegtestNode::RecordTransportResult(const net::TransportResult& result) noexcept
{
    observability::SaturatingAdd(metrics_.p2p_messages_dispatched,
                                 static_cast<std::uint64_t>(result.messages_dispatched));
    observability::SaturatingAdd(metrics_.p2p_disconnects,
                                 static_cast<std::uint64_t>(result.disconnected));
    if (result.error != net::TransportError::kNone) {
        observability::SaturatingAdd(metrics_.p2p_transport_errors, 1U);
    }
}

void RegtestNode::RelayBlock(const primitives::Block& block) noexcept
{
    for (auto& peer : peers_) {
        if (peer.get().ReceiveBlock(block) == RegtestNodeError::kNone) {
            peer.get().RelayBlock(block);
        }
    }
}

void RegtestNode::RelayTransaction(const primitives::Transaction& transaction) noexcept
{
    for (auto& peer : peers_) {
        if (peer.get().ReceiveTransaction(transaction) == RegtestNodeError::kNone) {
            peer.get().RelayTransaction(transaction);
        }
    }
}

void RegtestNode::Log(const std::string& message) const noexcept
{
    try {
        std::ofstream stream{config_.log_path, std::ios::app};
        if (stream.is_open()) {
            stream << config_.name << ' ' << message << '\n';
        }
    } catch (...) {
    }
}

} // namespace nova::node
