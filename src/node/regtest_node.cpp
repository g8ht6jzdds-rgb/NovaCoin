#include "node/regtest_node.hpp"

#include "consensus/monetary.hpp"
#include "node/network_selection.hpp"

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

[[nodiscard]] chain::BlockValidationParams
ValidationParams(const consensus::NetworkParams& network) noexcept
{
    return {network.block_limits,
            network.pow,
            network.monetary,
            0U,
            500'000'000U,
            std::numeric_limits<std::uint32_t>::max(),
            1U,
            600U};
}

[[nodiscard]] std::optional<chain::ChainAnchor>
GenesisAnchor(const consensus::NetworkParams& network) noexcept
{
    const auto target =
        consensus::TargetFromCompact(network.genesis_block.header.bits, network.pow);
    const auto work =
        target.target.has_value() ? consensus::CalculateWork(*target.target) : std::nullopt;
    if (!target.target.has_value() || !work.has_value()) {
        return std::nullopt;
    }
    return chain::ChainAnchor{network.genesis_hash,
                              crypto::Hash256{},
                              0U,
                              network.genesis_block.header.time,
                              *target.target,
                              *work,
                              chain::ChainWork::FromPerBlockWork(*work)};
}

[[nodiscard]] std::vector<std::uint8_t> P2pkhScript(const crypto::Hash160& hash)
{
    std::vector<std::uint8_t> script{0x76U, 0xA9U, 0x14U};
    script.insert(script.end(), hash.begin(), hash.end());
    script.insert(script.end(), {0x88U, 0xACU});
    return script;
}

[[nodiscard]] std::vector<std::uint8_t> CoinbaseHeightScript(const std::uint32_t height)
{
    if (height <= 127U) {
        return {0x01U, static_cast<std::uint8_t>(height)};
    }
    return {0x02U, static_cast<std::uint8_t>(height & 0xFFU),
            static_cast<std::uint8_t>((height >> 8U) & 0xFFU)};
}

} // namespace

RegtestNode::RegtestNode(RegtestNodeConfig config, storage::BlockJournal journal,
                         std::unique_ptr<chain::UTXOSet> utxos,
                         std::unique_ptr<chain::ChainState> chain,
                         std::unique_ptr<chain::Mempool> mempool,
                         std::unique_ptr<wallet::Wallet> wallet,
                         std::shared_ptr<const chain::ValidationTimeSource> validation_time_source,
                         const consensus::NetworkParams& network) noexcept
    : config_(std::move(config)), journal_(std::move(journal)), utxos_(std::move(utxos)),
      chain_(std::move(chain)), mempool_(std::move(mempool)), wallet_(std::move(wallet)),
      validation_time_source_(std::move(validation_time_source)), network_(&network),
      next_time_(config_.start_time)
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
        auto journal = storage::BlockJournal::Open(config.data_directory, network.block_limits);
        const auto anchor = GenesisAnchor(network);
        const wallet::WalletParams wallet_parameters{network.block_limits.transaction_limits, 1, 1U,
                                                     true};
        const auto wallet_path = config.data_directory / "wallet.dat";
        std::error_code wallet_error;
        const bool wallet_exists = std::filesystem::exists(wallet_path, wallet_error);
        if (wallet_error || (wallet_exists && config.wallet_passphrase.empty())) {
            return nullptr;
        }
        auto wallet = wallet_exists ? wallet::Wallet::LoadEncrypted(wallet_parameters, wallet_path,
                                                                    config.wallet_passphrase)
                                    : wallet::Wallet::Create(wallet_parameters);
        auto mempool = chain::Mempool::Create(
            chain::MempoolParams{ValidationParams(network), 1'000U, 10'000'000U, 0});
        if (!journal.has_value() || !anchor.has_value() || wallet == nullptr ||
            mempool == nullptr) {
            return nullptr;
        }
        auto validation_time_source = std::make_shared<chain::SystemValidationTimeSource>();
        auto utxos = std::make_unique<chain::UTXOSet>();
        auto chain = chain::ChainState::Create(
            {ValidationParams(network), network.difficulty, 11U, validation_time_source}, *anchor,
            *utxos);
        if (chain == nullptr) {
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
            std::move(config), std::move(*journal), std::move(utxos), std::move(chain),
            std::move(mempool), std::move(wallet), std::move(validation_time_source), network}};
        if (!node->PersistWallet()) {
            return nullptr;
        }
        const auto saved = node->journal_.Load();
        if (saved.error != storage::BlockJournalError::kNone) {
            return nullptr;
        }
        for (const auto& block : saved.blocks) {
            if (node->AcceptBlock(block, false) != RegtestNodeError::kNone) {
                return nullptr;
            }
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
    const auto built = wallet_->CreateTransaction(recipients);
    if (!built.built.has_value()) {
        return {RegtestNodeError::kWalletFailure, std::nullopt, std::nullopt};
    }
    if (SubmitTransaction(built.built->transaction) != RegtestNodeError::kNone) {
        return {RegtestNodeError::kMempoolFailure, std::nullopt, std::nullopt};
    }
    (void)wallet_->ObserveTransaction(built.built->transaction, 0U, false);
    if (!PersistWallet()) {
        return {RegtestNodeError::kWalletFailure, std::nullopt, std::nullopt};
    }
    return {RegtestNodeError::kNone, std::nullopt, built.built->transaction};
}

RegtestNodeError RegtestNode::ReceiveBlock(const primitives::Block& block) noexcept
{
    const auto result = AcceptBlock(block, true);
    if (result == RegtestNodeError::kNone) {
        observability::SaturatingAdd(metrics_.blocks_accepted, 1U);
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
    if (persist && journal_.Prepare(block) != storage::BlockJournalError::kNone) {
        return RegtestNodeError::kStorageFailure;
    }
    const auto accepted = chain_->AcceptBlock(block);
    if (accepted.error != chain::ChainError::kNone) {
        return RegtestNodeError::kBlockRejected;
    }
    if (persist && journal_.Commit(*hash) != storage::BlockJournalError::kNone) {
        // A durable record can exist even if the writer reports an error after
        // its final flush. Resolve that ambiguity from the authoritative WAL
        // before deciding whether to roll the in-memory chain back.
        if (!journal_.IsCommitted(*hash)) {
            const auto rollback = chain_->RollbackAcceptedBlock(*hash, active_tip_before);
            if (rollback.error != chain::ChainError::kNone) {
                return RegtestNodeError::kChainInitializationFailure;
            }
            return RegtestNodeError::kStorageFailure;
        }
    }
    const auto next_block_time = block.header.time + 1U;
    next_time_ = std::max(next_time_, next_block_time);
    if (!accepted.became_active) {
        Log("inactive block accepted");
        return RegtestNodeError::kNone;
    }
    wallet_->ClearConfirmedState();
    const auto active = chain_->GetActiveBlockIndexes();
    if (!active.has_value()) {
        return RegtestNodeError::kChainInitializationFailure;
    }
    for (const auto& index : *active) {
        if (!index.block.has_value()) {
            continue;
        }
        for (const auto& transaction : index.block->transactions) {
            (void)wallet_->ObserveTransaction(transaction, index.height, true);
        }
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
    if (!PersistWallet()) {
        return RegtestNodeError::kWalletFailure;
    }
    Log("block accepted");
    return RegtestNodeError::kNone;
}

RegtestNodeError
RegtestNode::ReceiveTransaction(const primitives::Transaction& transaction) noexcept
{
    const auto accepted =
        mempool_->AcceptToMempool(transaction, {height() + 1U, next_time_}, *utxos_, next_time_);
    if (accepted.error != chain::MempoolError::kNone) {
        observability::SaturatingAdd(metrics_.transactions_rejected, 1U);
        return RegtestNodeError::kMempoolFailure;
    }
    try {
        known_transactions_.push_back(transaction);
        if (!PersistWallet()) {
            observability::SaturatingAdd(metrics_.transactions_rejected, 1U);
            return RegtestNodeError::kWalletFailure;
        }
        observability::SaturatingAdd(metrics_.transactions_accepted, 1U);
        return RegtestNodeError::kNone;
    } catch (...) {
        observability::SaturatingAdd(metrics_.transactions_rejected, 1U);
        return RegtestNodeError::kAllocationFailure;
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
    return metrics_;
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
