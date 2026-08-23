#pragma once

#include "chain/chainstate.hpp"
#include "chain/mempool.hpp"
#include "consensus/network_params.hpp"
#include "net/transport.hpp"
#include "observability/metrics.hpp"
#include "storage/block_journal.hpp"
#include "wallet/wallet.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nova::node
{

struct RegtestNodeConfig final {
    std::string name;
    std::filesystem::path data_directory;
    std::filesystem::path log_path;
    std::uint16_t p2p_port{};
    std::uint16_t rpc_port{};
    std::uint32_t start_time{};
    // Supplied out-of-band by the daemon environment. It is never logged or
    // exposed by RPC. An empty value selects explicitly ephemeral wallet state.
    std::vector<std::uint8_t> wallet_passphrase;
    consensus::NetworkId network{consensus::NetworkId::kRegtest};
};

enum class RegtestNodeError : std::uint8_t {
    kNone,
    kInvalidConfiguration,
    kStorageFailure,
    kChainInitializationFailure,
    kReplayFailure,
    kWalletFailure,
    kMempoolFailure,
    kMiningFailure,
    kBlockRejected,
    kDuplicateBlock,
    kAllocationFailure,
};

struct RegtestNodeResult final {
    RegtestNodeError error{RegtestNodeError::kNone};
    std::optional<crypto::Hash256> block_hash;
    std::optional<primitives::Transaction> transaction;
};

class RegtestNode final
{
  public:
    RegtestNode(const RegtestNode&) = delete;
    RegtestNode& operator=(const RegtestNode&) = delete;

    [[nodiscard]] static std::unique_ptr<RegtestNode> Create(RegtestNodeConfig config) noexcept;
    [[nodiscard]] bool Connect(RegtestNode& peer) noexcept;
    void DisconnectAll() noexcept;
    [[nodiscard]] RegtestNodeResult MineBlock() noexcept;
    [[nodiscard]] RegtestNodeResult
    CreateAndBroadcast(std::span<const wallet::Recipient> recipients) noexcept;
    [[nodiscard]] RegtestNodeError ReceiveBlock(const primitives::Block& block) noexcept;
    [[nodiscard]] RegtestNodeError
    ReceiveTransaction(const primitives::Transaction& transaction) noexcept;
    // Application adapters use this path so a local RPC submission has the
    // same mempool admission and relay behavior as a locally built payment.
    [[nodiscard]] RegtestNodeError
    SubmitTransaction(const primitives::Transaction& transaction) noexcept;

    [[nodiscard]] std::uint32_t height() const noexcept;
    // Exposes the deterministic local candidate timestamp for status and
    // regression tests; it is not derived from remote block data on failure.
    [[nodiscard]] std::uint32_t next_mining_time() const noexcept;
    [[nodiscard]] const crypto::Hash256& tip() const noexcept;
    [[nodiscard]] std::size_t mempool_size() const noexcept;
    [[nodiscard]] const chain::ChainState& chain_state() const noexcept;
    [[nodiscard]] chain::ChainState& chain_state() noexcept;
    [[nodiscard]] chain::UTXOSet& utxos() noexcept;
    [[nodiscard]] chain::Mempool& mempool() noexcept;
    [[nodiscard]] wallet::Wallet& wallet() noexcept;
    [[nodiscard]] const wallet::Wallet& wallet() const noexcept;
    [[nodiscard]] bool PersistWalletForRpc() const noexcept;
    [[nodiscard]] const RegtestNodeConfig& config() const noexcept;
    [[nodiscard]] const consensus::NetworkParams& network_params() const noexcept;
    [[nodiscard]] observability::MetricsSnapshot metrics() const noexcept;
    void RecordTransportResult(const net::TransportResult& result) noexcept;

  private:
    RegtestNode(RegtestNodeConfig config, storage::BlockJournal journal,
                std::unique_ptr<chain::UTXOSet> utxos, std::unique_ptr<chain::ChainState> chain,
                std::unique_ptr<chain::Mempool> mempool, std::unique_ptr<wallet::Wallet> wallet,
                std::shared_ptr<const chain::ValidationTimeSource> validation_time_source,
                const consensus::NetworkParams& network) noexcept;

    [[nodiscard]] RegtestNodeError AcceptBlock(const primitives::Block& block,
                                               bool persist) noexcept;
    [[nodiscard]] bool PersistWallet() const noexcept;
    void RelayBlock(const primitives::Block& block) noexcept;
    void RelayTransaction(const primitives::Transaction& transaction) noexcept;
    void Log(const std::string& message) const noexcept;

    RegtestNodeConfig config_;
    storage::BlockJournal journal_;
    std::unique_ptr<chain::UTXOSet> utxos_;
    std::unique_ptr<chain::ChainState> chain_;
    std::unique_ptr<chain::Mempool> mempool_;
    std::unique_ptr<wallet::Wallet> wallet_;
    std::shared_ptr<const chain::ValidationTimeSource> validation_time_source_;
    const consensus::NetworkParams* network_{};
    std::vector<std::reference_wrapper<RegtestNode>> peers_;
    std::vector<primitives::Transaction> known_transactions_;
    std::uint32_t next_time_{};
    observability::MetricsSnapshot metrics_;
};

} // namespace nova::node
