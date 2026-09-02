#pragma once

#include "chain/chainstate.hpp"
#include "consensus/network_params.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

namespace nova::explorer
{

// A source is intentionally read-only. It supplies copies, so the explorer
// cannot mutate a node's chain state, mempool, or UTXO set.
struct ExplorerBlock final {
    std::uint32_t height{};
    crypto::Hash256 hash;
    primitives::Block block;
};

struct ExplorerSnapshot final {
    consensus::NetworkId network{consensus::NetworkId::kRegtest};
    std::uint32_t target_spacing_seconds{};
    consensus::PowParameters pow_parameters;
    primitives::BlockLimits block_limits;
    std::vector<ExplorerBlock> active_blocks;
};

class ExplorerSource
{
  public:
    virtual ~ExplorerSource() = default;
    [[nodiscard]] virtual std::optional<ExplorerSnapshot> ReadSnapshot() const noexcept = 0;
};

// This is the novacoind integration boundary. It holds const references and
// only requests copied active-chain data.
class NovacoindSnapshotSource final : public ExplorerSource
{
  public:
    NovacoindSnapshotSource(const chain::ChainState& chain,
                            const consensus::NetworkParams& parameters) noexcept;

    [[nodiscard]] std::optional<ExplorerSnapshot> ReadSnapshot() const noexcept override;

  private:
    const chain::ChainState& chain_;
    const consensus::NetworkParams& parameters_;
};

// An independent-process source. It has no node or consensus-state reference:
// it obtains a bounded copied snapshot through authenticated localhost RPC.
struct RpcSnapshotConfig final {
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
    std::string username;
    std::string password;
    std::size_t max_response_bytes{};
    const consensus::NetworkParams* network{};
};

class AuthenticatedRpcSnapshotSource final : public ExplorerSource
{
  public:
    AuthenticatedRpcSnapshotSource(const AuthenticatedRpcSnapshotSource&) = delete;
    AuthenticatedRpcSnapshotSource& operator=(const AuthenticatedRpcSnapshotSource&) = delete;

    [[nodiscard]] static std::unique_ptr<AuthenticatedRpcSnapshotSource>
    Create(RpcSnapshotConfig config) noexcept;
    [[nodiscard]] std::optional<ExplorerSnapshot> ReadSnapshot() const noexcept override;

  private:
    explicit AuthenticatedRpcSnapshotSource(RpcSnapshotConfig config) noexcept;

    RpcSnapshotConfig config_;
};

struct ExplorerSummary final {
    consensus::NetworkId network{consensus::NetworkId::kRegtest};
    std::uint32_t height{};
    crypto::Hash256 best_block;
    consensus::Target256 target;
    std::uint32_t target_spacing_seconds{};
    std::uint64_t estimated_blocks_per_day{};
    std::size_t indexed_blocks{};
    std::size_t indexed_transactions{};
    std::size_t unspent_outputs{};
};

struct ExplorerTransaction final {
    crypto::Hash256 transaction_id;
    crypto::Hash256 block_hash;
    std::uint32_t block_height{};
    primitives::Transaction transaction;
    bool is_coinbase{};
    std::optional<primitives::Amount> fee;
};

struct ExplorerUtxo final {
    chain::UTXOKey key;
    chain::Coin coin;
    std::optional<crypto::Hash160> address;
};

enum class ExplorerError : std::uint8_t {
    kNone,
    kEmptySnapshot,
    kInvalidLimits,
    kInvalidBlockHeight,
    kInvalidBlockHash,
    kInvalidBlockLink,
    kInvalidBlockStructure,
    kDuplicateTransaction,
    kMissingInput,
    kInputAmountOverflow,
    kOutputAmountOverflow,
    kNegativeFee,
    kAllocationFailure,
};

struct ExplorerResult final {
    ExplorerError error{ExplorerError::kNone};
};

class ExplorerIndex final
{
  public:
    ExplorerIndex() = default;
    ExplorerIndex(const ExplorerIndex&) = delete;
    ExplorerIndex& operator=(const ExplorerIndex&) = delete;
    ExplorerIndex(ExplorerIndex&&) = delete;
    ExplorerIndex& operator=(ExplorerIndex&&) = delete;

    // A rejected snapshot leaves the previously published index unchanged.
    [[nodiscard]] ExplorerResult Rebuild(const ExplorerSnapshot& snapshot) noexcept;

    [[nodiscard]] std::optional<ExplorerSummary> Summary() const noexcept;
    [[nodiscard]] std::optional<primitives::TransactionLimits> TransactionLimits() const noexcept;
    [[nodiscard]] std::vector<ExplorerBlock> LatestBlocks(std::size_t count) const;
    [[nodiscard]] std::optional<ExplorerBlock>
    FindBlockByHeight(std::uint32_t height) const noexcept;
    [[nodiscard]] std::optional<ExplorerBlock>
    FindBlockByHash(const crypto::Hash256& hash) const noexcept;
    [[nodiscard]] std::optional<ExplorerTransaction>
    FindTransaction(const crypto::Hash256& transaction_id) const noexcept;
    [[nodiscard]] std::vector<ExplorerUtxo> AllUtxos(std::size_t maximum) const;
    [[nodiscard]] std::vector<ExplorerUtxo> FindUtxosByAddress(const crypto::Hash160& address,
                                                               std::size_t maximum) const;

  private:
    struct HashLess final {
        [[nodiscard]] bool operator()(const crypto::Hash256& left,
                                      const crypto::Hash256& right) const noexcept;
    };
    using BlocksByHash = std::map<crypto::Hash256, ExplorerBlock, HashLess>;
    using Transactions = std::map<crypto::Hash256, ExplorerTransaction, HashLess>;
    using Utxos = std::map<chain::UTXOKey, chain::Coin>;

    std::optional<ExplorerSummary> summary_;
    std::optional<primitives::TransactionLimits> transaction_limits_;
    mutable std::shared_mutex mutex_;
    std::vector<crypto::Hash256> block_order_;
    BlocksByHash blocks_;
    Transactions transactions_;
    Utxos utxos_;
};

// NovaCoin v0 recognizes a displayed address only for the exact standard
// P2PKH script: OP_DUP OP_HASH160 PUSH20 <hash160> OP_EQUALVERIFY OP_CHECKSIG.
[[nodiscard]] std::optional<crypto::Hash160>
ExtractP2pkhAddress(std::span<const std::uint8_t> script_pubkey) noexcept;

} // namespace nova::explorer
