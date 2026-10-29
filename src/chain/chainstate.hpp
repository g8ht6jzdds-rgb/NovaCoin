#pragma once

#include "chain/validation.hpp"
#include "consensus/network_params.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace nova::chain
{

class ChainWork final
{
  public:
    static constexpr std::size_t kSize = 64U;
    using Bytes = std::array<std::uint8_t, kSize>;

    ChainWork() = default;
    explicit ChainWork(Bytes bytes) noexcept;

    [[nodiscard]] static ChainWork FromPerBlockWork(const consensus::Target256& work) noexcept;
    [[nodiscard]] bool Add(const consensus::Target256& work) noexcept;
    [[nodiscard]] const Bytes& bytes() const noexcept;

    friend bool operator==(const ChainWork&, const ChainWork&) = default;
    friend bool operator<(const ChainWork& left, const ChainWork& right) noexcept;

  private:
    Bytes bytes_{};
};

// Wall-clock time is non-consensus context, but it is an authoritative node
// input: candidate blocks must never be able to choose it.
class ValidationTimeSource
{
  public:
    virtual ~ValidationTimeSource() = default;
    [[nodiscard]] virtual std::uint32_t Now() const noexcept = 0;
};

class FixedValidationTimeSource final : public ValidationTimeSource
{
  public:
    explicit FixedValidationTimeSource(std::uint32_t now) noexcept : now_(now) {}

    [[nodiscard]] std::uint32_t Now() const noexcept override
    {
        return now_;
    }
    void SetNow(std::uint32_t now) noexcept
    {
        now_ = now;
    }

  private:
    std::uint32_t now_{};
};

// Production nodes obtain validation time solely from the local system clock.
// FixedValidationTimeSource is restricted to deterministic tests and explicit
// harness injection; it must never be selected by normal node startup.
class SystemValidationTimeSource final : public ValidationTimeSource
{
  public:
    [[nodiscard]] std::uint32_t Now() const noexcept override;
};

enum class BlockStatus : std::uint8_t {
    kPending,
    kValid,
    kActive,
    kInvalid,
};

struct BlockIndex final {
    crypto::Hash256 hash;
    crypto::Hash256 previous_hash;
    std::uint32_t height{};
    std::uint32_t time{};
    std::uint32_t expected_bits{};
    consensus::Target256 target;
    consensus::Target256 per_block_work;
    ChainWork chain_work;
    BlockStatus status{BlockStatus::kPending};
    std::optional<primitives::Block> block;
    std::optional<ConnectedBlock> connected;
    std::uint32_t validation_time{};
};

class Chain final
{
  public:
    Chain() = default;
    Chain(const crypto::Hash256& tip, std::uint32_t height) noexcept;

    void SetTip(const crypto::Hash256& tip, std::uint32_t height) noexcept;
    [[nodiscard]] const crypto::Hash256& tip() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;

  private:
    crypto::Hash256 tip_;
    std::uint32_t height_{};
};

// Fixed-size, read-only export for node-local synchronization. No block payload,
// UTXO undo data or allocation; absence means no usable validated index.
struct HeaderMetadata final {
    crypto::Hash256 hash;
    crypto::Hash256 previous_hash;
    std::uint32_t height{};
    std::uint32_t time{};
    std::uint32_t bits{};
    consensus::Target256 target;
    consensus::Target256 work;
    ChainWork chain_work;
};

struct ChainAnchor final {
    crypto::Hash256 hash;
    crypto::Hash256 previous_hash;
    std::uint32_t height{};
    std::uint32_t time{};
    consensus::Target256 target;
    consensus::Target256 per_block_work;
    ChainWork chain_work;
};

struct ChainStateParams final {
    BlockValidationParams block_validation;
    consensus::DifficultyParameters difficulty;
    std::uint32_t median_time_past_window{};
    std::shared_ptr<const ValidationTimeSource> validation_time_source;
};

enum class ChainError : std::uint8_t {
    kNone,
    kInvalidAnchor,
    kInvalidParameters,
    kUnknownBlock,
    kUnknownParent,
    kDuplicateBlock,
    kHeightOverflow,
    kBlockHashFailure,
    kInvalidTarget,
    kWorkCalculationFailure,
    kChainWorkOverflow,
    kContextFailure,
    kBlockValidationFailure,
    kStateDisconnectFailure,
    kStateConnectFailure,
    kReorganizationFailure,
    kRollbackFailure,
    kAllocationFailure,
};

struct ChainResult final {
    ChainError error{ChainError::kNone};
    BlockValidationError validation_error{BlockValidationError::kNone};
    std::optional<crypto::Hash256> block_hash;
    // Set only by AcceptBlock when this specific block is part of the active
    // chain after all chainwork selection and any reorganization complete.
    bool became_active{};
};

class ChainState final
{
  private:
    struct HashLess final {
        [[nodiscard]] bool operator()(const crypto::Hash256& left,
                                      const crypto::Hash256& right) const noexcept;
    };

    using Indexes = std::map<crypto::Hash256, BlockIndex, HashLess>;

  public:
    ChainState(const ChainState&) = delete;
    ChainState& operator=(const ChainState&) = delete;
    // Prepared transitions borrow indexes; the live owner stays at its address.
    ChainState(ChainState&&) = delete;
    ChainState& operator=(ChainState&&) = delete;

    [[nodiscard]] static std::unique_ptr<ChainState>
    Create(ChainStateParams parameters, ChainAnchor anchor, UTXOSet& utxos) noexcept;

    // Per-instance one-shot fault seam; never configured from network input.
    enum class FailurePoint : std::uint8_t { kNone, kBeforeStaging, kAfterCopy, kAfterTransition };
    void SetFailurePointForTesting(FailurePoint point) noexcept;

    class PreparedTransition;
    struct PreparationResult;
    // Exclusive, non-reentrant ownership; UTXOSet must outlive ChainState.
    // Prepared state does not publish or imply durable acceptance.
    [[nodiscard]] PreparationResult PrepareBlock(const primitives::Block& block) noexcept;
    [[nodiscard]] bool CanCommitPrepared(const PreparedTransition& prepared) const noexcept;
    [[nodiscard]] ChainResult CommitPrepared(PreparedTransition& prepared) noexcept;
    // Visitor receives only a lexical const borrow and returns bool success.
    // Keep owner/token/UTXO alive, do not retain the view or publish live state.
    // On false (including stale-after-visit), discard all prepared outputs.
    template <typename Visitor>
    [[nodiscard]] bool WithPreparedUTXO(const PreparedTransition& prepared,
                                        Visitor&& visitor) const noexcept;
    [[nodiscard]] std::optional<std::vector<BlockIndex>>
    GetPreparedActiveBlockIndexes(const PreparedTransition& prepared) const noexcept;

    [[nodiscard]] ChainResult AcceptBlock(const primitives::Block& block) noexcept;
    // Used only to roll back a staged journal acceptance that could not be
    // durably committed. Callers must provide the immediately preceding tip.
    [[nodiscard]] ChainResult RollbackAcceptedBlock(const crypto::Hash256& block_hash,
                                                    const crypto::Hash256& previous_tip) noexcept;
    [[nodiscard]] ChainResult ActivateBestChain() noexcept;
    [[nodiscard]] std::optional<crypto::Hash256>
    FindFork(const crypto::Hash256& left, const crypto::Hash256& right) const noexcept;

    [[nodiscard]] const crypto::Hash256& active_tip() const noexcept;
    [[nodiscard]] std::uint32_t active_height() const noexcept;
    [[nodiscard]] std::size_t known_block_count() const noexcept;
    [[nodiscard]] std::optional<BlockIndex>
    GetBlockIndex(const crypto::Hash256& hash) const noexcept;
    [[nodiscard]] std::optional<HeaderMetadata>
    GetHeaderMetadata(const crypto::Hash256& hash) const noexcept;
    // Returns copied active-chain entries ordered by height for non-consensus
    // consumers.  Callers cannot use this read-only export to mutate state.
    [[nodiscard]] std::optional<std::vector<BlockIndex>> GetActiveBlockIndexes() const noexcept;

    // `Indexes` is private, so callers can only construct through Create.
    // The constructor is public solely to permit std::make_unique in Create.
    ChainState(ChainStateParams parameters, UTXOSet& utxos, Indexes indexes,
               const crypto::Hash256& active_tip, std::uint32_t active_height);

  private:
    friend class NetworkChainInitializer;

    template <typename Operation>
    [[nodiscard]] PreparationResult
    PrepareOperation(Operation&& operation, std::optional<crypto::Hash256> failure_hash) noexcept;
    template <typename Operation>
    [[nodiscard]] ChainResult ApplyAtomically(Operation&& operation,
                                              std::optional<crypto::Hash256> failure_hash) noexcept;
    [[nodiscard]] ChainResult AcceptBlockStaged(const primitives::Block& block) noexcept;
    [[nodiscard]] ChainResult ActivateBestChainStaged() noexcept;
    [[nodiscard]] ChainResult
    RollbackAcceptedBlockStaged(const crypto::Hash256& block_hash,
                                const crypto::Hash256& previous_tip) noexcept;

    [[nodiscard]] std::optional<BlockValidationContext>
    BuildContext(const BlockIndex& parent, std::uint32_t candidate_time) const;
    [[nodiscard]] std::optional<std::uint32_t>
    ExpectedBits(const BlockIndex& parent, std::uint32_t candidate_time) const noexcept;
    [[nodiscard]] const BlockIndex* Ancestor(const BlockIndex& start,
                                             std::uint32_t height) const noexcept;
    [[nodiscard]] ChainResult ReorganizeChain(const crypto::Hash256& new_tip) noexcept;
    [[nodiscard]] ChainResult ConnectIndexedBlock(BlockIndex& index) noexcept;
    [[nodiscard]] ChainError DisconnectIndexedBlock(BlockIndex& index) noexcept;
    void MarkActiveChain();
    [[nodiscard]] const BlockIndex* FindIndex(const crypto::Hash256& hash) const noexcept;
    [[nodiscard]] BlockIndex* EditIndex(const crypto::Hash256& hash);
    void EraseIndex(const crypto::Hash256& hash);
    template <typename Visitor> void ForEachIndex(Visitor&& visitor) const;

    std::shared_ptr<const std::uint8_t> identity_;
    std::uint64_t publication_revision_{};
    ChainStateParams parameters_;
    UTXOSet& utxos_;
    UTXOBatch* transition_utxos_{};
    // A private transition stores only replacements/new entries here.
    Indexes indexes_;
    const Indexes* base_indexes_{};
    std::set<crypto::Hash256, HashLess> erased_indexes_;
    Chain active_chain_;
    FailurePoint failure_point_{FailurePoint::kNone};
};

class ChainState::PreparedTransition final
{
  public:
    PreparedTransition(const PreparedTransition&) = delete;
    PreparedTransition& operator=(const PreparedTransition&) = delete;
    PreparedTransition(PreparedTransition&&) = delete;
    PreparedTransition& operator=(PreparedTransition&&) = delete;

  private:
    PreparedTransition() = default;
    std::weak_ptr<const std::uint8_t> owner_;
    std::uint64_t revision_{};
    std::optional<UTXOBatch> utxos_;
    std::unique_ptr<ChainState> staged_;
    ChainResult result_;
    bool consumed_{};
    friend class ChainState;
};

struct ChainState::PreparationResult final {
    ChainResult result;
    std::unique_ptr<PreparedTransition> prepared;
};

template <typename Visitor>
bool ChainState::WithPreparedUTXO(const PreparedTransition& prepared,
                                  Visitor&& visitor) const noexcept
{
    if (!CanCommitPrepared(prepared))
        return false;
    try {
        const bool success =
            std::forward<Visitor>(visitor)(static_cast<const UTXOView&>(*prepared.utxos_));
        return success && CanCommitPrepared(prepared);
    } catch (...) {
        return false;
    }
}

} // namespace nova::chain
