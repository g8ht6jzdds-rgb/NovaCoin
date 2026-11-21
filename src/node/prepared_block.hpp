#pragma once

#include "chain/chainstate.hpp"
#include "chain/mempool.hpp"
#include "storage/generation_store.hpp"
#include "wallet/wallet.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace nova::node
{

enum class BlockPreparationError : std::uint8_t {
    kNone,
    kChain,
    kWallet,
    kMempool,
    kContext,
    kAllocation
};

enum class GenerationStageError : std::uint8_t {
    kNone,
    kStale,
    kInvalidBase,
    kFilesystem,
    kJournal,
    kWallet,
    kStore
};

struct BlockPreparationResult;
class RecoveredGeneration;
class PreparedNodeGeneration;
enum class NodePublicationResult : std::uint8_t { kCommitted, kNotCommitted, kStale, kUnavailable };

// A private candidate only: it does not commit a journal, publish state or
// imply that the block was durably accepted. Owners must outlive this object
// and remain exclusively held through preparation, staging and publication.
class PreparedNodeBlock final
{
  public:
    PreparedNodeBlock(const PreparedNodeBlock&) = delete;
    PreparedNodeBlock& operator=(const PreparedNodeBlock&) = delete;
    [[nodiscard]] const chain::ChainResult& result() const noexcept
    {
        return result_;
    }
    [[nodiscard]] std::uint32_t next_time() const noexcept
    {
        return next_time_;
    }
    [[nodiscard]] bool
    CanPublish(const chain::ChainState& chain, const wallet::Wallet& wallet,
               const chain::Mempool& mempool, std::uint32_t current_time,
               const wallet::Wallet::PreparedAddress* address = nullptr) const noexcept;
    // Requires the exact recovered objects and store to remain unchanged.
    [[nodiscard]] std::unique_ptr<PreparedNodeGeneration> StageRecoveredGeneration(
        storage::GenerationStore& store, RecoveredGeneration& recovered,
        const chain::Mempool& mempool, std::uint32_t current_time,
        const std::filesystem::path& fresh_directory, std::span<const std::uint8_t> passphrase,
        GenerationStageError& error,
        const wallet::Wallet::PreparedAddress* address = nullptr) const noexcept;
    // kUnavailable requires quarantining all node access until recovery.
    [[nodiscard]] NodePublicationResult
    CommitGeneration(PreparedNodeGeneration& generation, storage::GenerationStore& store,
                     RecoveredGeneration& recovered, chain::Mempool& mempool,
                     wallet::Wallet::PreparedAddress* address = nullptr) noexcept;

  private:
    PreparedNodeBlock() = default;
    [[nodiscard]] std::unique_ptr<PreparedNodeGeneration>
    StageGeneration(storage::GenerationStore& store, const storage::GenerationSnapshot& expected,
                    const chain::ChainState& chain, const wallet::Wallet& wallet,
                    const chain::Mempool& mempool, std::uint32_t current_time,
                    const std::filesystem::path& fresh_directory, primitives::BlockLimits limits,
                    std::span<const std::uint8_t> passphrase, GenerationStageError& error,
                    const wallet::Wallet::PreparedAddress* address) const noexcept;
    std::shared_ptr<const std::uint8_t> identity_;
    chain::ChainResult result_;
    std::unique_ptr<primitives::Block> block_;
    std::unique_ptr<chain::ChainState::PreparedTransition> chain_;
    std::unique_ptr<wallet::Wallet::PreparedConfirmedState> wallet_;
    std::unique_ptr<chain::Mempool::PreparedState> mempool_;
    std::uint32_t prior_time_{};
    std::uint32_t next_time_{};
    friend BlockPreparationResult PrepareNodeBlock(chain::ChainState&, wallet::Wallet&,
                                                   chain::Mempool&, const primitives::Block&,
                                                   std::span<const primitives::Transaction>,
                                                   std::uint32_t,
                                                   const wallet::Wallet::PreparedAddress*) noexcept;
};

class PreparedNodeGeneration final
{
  public:
    PreparedNodeGeneration(const PreparedNodeGeneration&) = delete;
    PreparedNodeGeneration& operator=(const PreparedNodeGeneration&) = delete;

  private:
    PreparedNodeGeneration() = default;
    std::weak_ptr<const std::uint8_t> owner_;
    std::unique_ptr<storage::GenerationStore::Prepared> generation_;
    bool attempted_{};
    bool finished_{};
    bool publication_failed_{};
    friend class PreparedNodeBlock;
};

struct BlockPreparationResult final {
    BlockPreparationError error{BlockPreparationError::kContext};
    chain::ChainResult chain_result;
    std::unique_ptr<PreparedNodeBlock> candidate;
};

[[nodiscard]] BlockPreparationResult
PrepareNodeBlock(chain::ChainState& chain, wallet::Wallet& wallet, chain::Mempool& mempool,
                 const primitives::Block& block, std::span<const primitives::Transaction> known,
                 std::uint32_t next_time,
                 const wallet::Wallet::PreparedAddress* address = nullptr) noexcept;

} // namespace nova::node
