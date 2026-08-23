#pragma once

#include "chain/validation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace nova::chain
{

struct MempoolParams final {
    BlockValidationParams validation;
    std::uint32_t max_transactions{};
    std::uint64_t max_total_serialized_size{};
    primitives::Amount min_fee{};
};

struct MempoolEntry final {
    crypto::Hash256 transaction_id;
    primitives::Transaction transaction;
    primitives::Amount fee{};
    std::uint64_t serialized_size{};
    primitives::Amount fee_rate{};
    std::uint64_t time_received{};
    std::vector<crypto::Hash256> dependencies;
};

enum class MempoolError : std::uint8_t {
    kNone,
    kInvalidParameters,
    kInvalidTransaction,
    kDuplicateTransaction,
    kMissingInput,
    kMempoolDoubleSpend,
    kFeeOutOfRange,
    kFeeBelowMinimum,
    kSizeOverflow,
    kPoolFull,
    kPoolSizeLimit,
    kStateUnavailable,
    kAllocationFailure,
};

struct MempoolResult final {
    MempoolError error{MempoolError::kNone};
    BlockValidationError validation_error{BlockValidationError::kNone};
    std::optional<crypto::Hash256> transaction_id;
    std::size_t removed_count{};
};

struct BlockTemplateResult final {
    MempoolError error{MempoolError::kNone};
    std::vector<primitives::Transaction> transactions;
};

class Mempool final
{
  private:
    struct HashLess final {
        [[nodiscard]] bool operator()(const crypto::Hash256& left,
                                      const crypto::Hash256& right) const noexcept;
    };

    using Entries = std::map<crypto::Hash256, MempoolEntry, HashLess>;
    using Spends = std::map<UTXOKey, crypto::Hash256>;

    struct ConstructionKey final {
    };
    struct RebuiltPool final {
        Entries entries;
        Spends spends;
        std::uint64_t total_serialized_size{};
    };

  public:
    Mempool(const Mempool&) = delete;
    Mempool& operator=(const Mempool&) = delete;
    Mempool(Mempool&&) noexcept = default;
    Mempool& operator=(Mempool&&) = delete;

    [[nodiscard]] static std::unique_ptr<Mempool> Create(MempoolParams parameters) noexcept;

    [[nodiscard]] MempoolResult AcceptToMempool(const primitives::Transaction& transaction,
                                                const TransactionValidationContext& context,
                                                UTXOSet& active_utxos,
                                                std::uint64_t time_received) noexcept;
    [[nodiscard]] MempoolResult RemoveTransaction(const crypto::Hash256& transaction_id) noexcept;
    [[nodiscard]] MempoolResult
    RemoveForBlock(std::span<const primitives::Transaction> block_transactions) noexcept;
    [[nodiscard]] MempoolResult RevalidateMempool(const TransactionValidationContext& context,
                                                  UTXOSet& active_utxos) noexcept;
    [[nodiscard]] BlockTemplateResult
    SelectForBlock(std::uint64_t max_serialized_size) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint64_t total_serialized_size() const noexcept;
    [[nodiscard]] std::optional<MempoolEntry>
    GetEntry(const crypto::Hash256& transaction_id) const noexcept;

    // The private construction key keeps construction routed through Create.
    Mempool(ConstructionKey, MempoolParams parameters) noexcept;

  private:
    [[nodiscard]] bool ParametersAreValid() const noexcept;
    [[nodiscard]] std::optional<UTXOBatch> BuildOverlay(const TransactionValidationContext& context,
                                                        UTXOSet& active_utxos) const noexcept;
    [[nodiscard]] std::optional<RebuiltPool>
    BuildRevalidatedPool(const TransactionValidationContext& context,
                         UTXOSet& active_utxos) const noexcept;
    [[nodiscard]] std::vector<crypto::Hash256>
    DependenciesFor(const primitives::Transaction& transaction, const Entries& entries) const;
    [[nodiscard]] static std::optional<std::uint64_t>
    SerializedSizeOf(const primitives::Transaction& transaction) noexcept;
    [[nodiscard]] static bool PreferredForBlock(const MempoolEntry& candidate,
                                                const MempoolEntry& current) noexcept;
    [[nodiscard]] static MempoolError MapValidationError(BlockValidationError error) noexcept;
    [[nodiscard]] static std::optional<Spends> BuildSpends(const Entries& entries) noexcept;

    MempoolParams parameters_;
    Entries entries_;
    Spends spends_;
    std::uint64_t total_serialized_size_{};
};

} // namespace nova::chain
