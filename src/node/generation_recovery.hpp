#pragma once

#include "chain/mempool_image.hpp"
#include "chain/network_chainstate.hpp"
#include "storage/generation_store.hpp"
#include "wallet/wallet.hpp"

#include <cstddef>

namespace nova::node
{

enum class GenerationRecoveryError : std::uint8_t {
    kNone,
    kNetwork,
    kUnavailable,
    kJournal,
    kChain,
    kWallet,
    kContext
};

struct GenerationRecoveryResult;

enum class GenerationRecoveryCheckpoint : std::uint8_t {
    kBeforeReplay,
    kAfterBlock,
    kBeforeWallet,
    kAfterWallet,
    kBeforeMempool,
    kAfterMempool,
    kBeforeAdmission
};

// Injected test dependency only; production callers leave this empty. No
// configuration/RPC selection, mutable node state or secret bytes are exposed.
// The context must outlive the synchronous recovery call.
struct GenerationRecoveryObserver final {
    const void* context{};
    void (*checkpoint)(const void*, GenerationRecoveryCheckpoint, std::size_t,
                       std::size_t) noexcept {};
    void Notify(GenerationRecoveryCheckpoint point, std::size_t completed,
                std::size_t total) const noexcept
    {
        if (checkpoint)
            checkpoint(context, point, completed, total);
    }
};

// Validates into a new private pool; never mutates the chain or wallet.
[[nodiscard]] std::unique_ptr<chain::Mempool>
ValidateRecoveredMempool(std::istream& input, const chain::MempoolParams& parameters,
                         const chain::TransactionValidationContext& context,
                         const chain::UTXOView& utxos, const wallet::Wallet& wallet,
                         std::span<const chain::BlockIndex> active,
                         bool allow_empty_anchor) noexcept;

// Fresh private objects only. The caller must retain the store lease and
// serialize all access. This is not a node-start or migration operation.
class RecoveredGeneration final
{
  public:
    RecoveredGeneration(const RecoveredGeneration&) = delete;
    RecoveredGeneration& operator=(const RecoveredGeneration&) = delete;
    [[nodiscard]] const storage::GenerationSnapshot& snapshot() const noexcept
    {
        return snapshot_;
    }
    [[nodiscard]] chain::ChainState& chain() noexcept
    {
        return *network_chain_.chain;
    }
    [[nodiscard]] chain::UTXOSet& utxos() noexcept
    {
        return *network_chain_.utxos;
    }
    [[nodiscard]] wallet::Wallet& wallet() noexcept
    {
        return *wallet_;
    }
    [[nodiscard]] std::uint32_t next_time() const noexcept
    {
        return next_time_;
    }
    [[nodiscard]] const primitives::BlockLimits& block_limits() const noexcept
    {
        return block_limits_;
    }
    [[nodiscard]] bool StillMatchesBase(std::uint32_t current_time) const noexcept;
    [[nodiscard]] bool MatchesStore(const storage::GenerationStore& store) const noexcept;
    // In-process test seam; not configurable through node or network input.
    void FailNextAdvanceForTesting() noexcept
    {
        fail_next_advance_ = true;
    }

  private:
    RecoveredGeneration() = default;
    [[nodiscard]] bool AdvanceAfterPublication(storage::GenerationStore& store,
                                               std::uint32_t next_time) noexcept;
    void Invalidate() noexcept
    {
        valid_ = false;
    }
    bool valid_{true};
    bool fail_next_advance_{};
    storage::GenerationSnapshot snapshot_;
    chain::InitializedNetworkChain network_chain_;
    std::unique_ptr<wallet::Wallet> wallet_;
    std::uint32_t next_time_{};
    primitives::BlockLimits block_limits_{};
    chain::ChainState::StateCheckpoint chain_checkpoint_;
    wallet::Wallet::StateCheckpoint wallet_checkpoint_;
    storage::GenerationStore::OwnerCheckpoint store_checkpoint_;
    // Destroy this borrowed UTXO batch before its owner in network_chain_.
    std::unique_ptr<chain::UTXOBatch> utxo_checkpoint_;
    friend class PreparedNodeBlock;
    friend class DurableSession;
    friend GenerationRecoveryResult
    RecoverGeneration(storage::GenerationStore&, consensus::NetworkId,
                      std::shared_ptr<const chain::ValidationTimeSource>, std::uint32_t,
                      std::span<const std::uint8_t>, GenerationRecoveryObserver) noexcept;
};

struct GenerationRecoveryResult final {
    GenerationRecoveryError error{GenerationRecoveryError::kUnavailable};
    std::unique_ptr<RecoveredGeneration> recovered;
    std::unique_ptr<chain::Mempool> mempool{};
};

[[nodiscard]] GenerationRecoveryResult
RecoverGeneration(storage::GenerationStore& store, consensus::NetworkId network,
                  std::shared_ptr<const chain::ValidationTimeSource> time_source,
                  std::uint32_t start_time, std::span<const std::uint8_t> passphrase,
                  GenerationRecoveryObserver observer = {}) noexcept;

} // namespace nova::node
