#pragma once

#include "primitives/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace nova::chain
{

struct Coin final {
    primitives::TxOutput output;
    std::uint32_t height{};
    bool is_coinbase{};
};

struct UTXOKey final {
    crypto::Hash256 transaction_id{};
    std::uint32_t output_index{};

    [[nodiscard]] static UTXOKey FromOutPoint(const primitives::OutPoint& outpoint) noexcept;

    friend bool operator==(const UTXOKey&, const UTXOKey&) = default;
    friend bool operator<(const UTXOKey& left, const UTXOKey& right) noexcept;
};

// A block-local overlay. It records only the entries touched while a batch is
// being validated; the durable UTXO map is not copied.
struct UTXODelta final {
    std::map<UTXOKey, Coin> additions;
    std::set<UTXOKey> spends;
};

class UTXOView
{
  public:
    virtual ~UTXOView() = default;

    [[nodiscard]] virtual std::optional<Coin> GetCoin(const UTXOKey& key) const = 0;
    [[nodiscard]] virtual bool HaveCoin(const UTXOKey& key) const noexcept = 0;
};

struct SpentCoin final {
    UTXOKey key;
    Coin coin;
};

struct TransactionUndo final {
    std::vector<SpentCoin> spent_coins;
    std::vector<UTXOKey> created_coins;
};

struct BlockUndo final {
    std::vector<TransactionUndo> transaction_undos;
};

enum class UTXOError : std::uint8_t {
    kNone,
    kInvalidTransactionStructure,
    kUnexpectedCoinbaseInput,
    kMissingInput,
    kDuplicateCoin,
    kOutputIndexOverflow,
    kUndoMissingCreatedCoin,
    kUndoExistingSpentCoin,
    kAllocationFailure,
    kBatchClosed,
};

struct ApplyTransactionResult final {
    UTXOError error{UTXOError::kNone};
    std::optional<TransactionUndo> undo;
};

struct ApplyBlockResult final {
    UTXOError error{UTXOError::kNone};
    std::optional<BlockUndo> undo;
};

class UTXOSet;

class UTXOBatch final : public UTXOView
{
  public:
    UTXOBatch(const UTXOBatch&) = delete;
    UTXOBatch& operator=(const UTXOBatch&) = delete;
    UTXOBatch(UTXOBatch&& other);
    UTXOBatch& operator=(UTXOBatch&&) noexcept = delete;

    [[nodiscard]] ApplyTransactionResult
    ApplyTransaction(const primitives::Transaction& transaction, std::uint32_t height,
                     bool is_coinbase, const primitives::TransactionLimits& limits) noexcept;
    [[nodiscard]] std::optional<Coin> GetCoin(const UTXOKey& key) const override;
    [[nodiscard]] bool HaveCoin(const UTXOKey& key) const noexcept override;
    [[nodiscard]] UTXOError UndoTransaction(const TransactionUndo& undo) noexcept;
    // Parent must outlive and remain at its address while a child exists.
    // A closed or changed parent cannot publish child changes.
    [[nodiscard]] std::optional<UTXOBatch> BeginBatch() noexcept;
    [[nodiscard]] UTXOError UndoBlock(const BlockUndo& undo) noexcept;
    [[nodiscard]] bool Commit() noexcept;
    void Rollback() noexcept;
    [[nodiscard]] std::size_t overlay_entry_count() const noexcept;

  private:
    explicit UTXOBatch(UTXOSet& base);
    explicit UTXOBatch(UTXOBatch& parent);
    [[nodiscard]] bool IsOpen() const noexcept;

    const UTXOView& base_;
    UTXOSet* base_set_{};
    UTXOBatch* parent_{};
    UTXODelta delta_;
    std::uint64_t revision_{};
    std::uint64_t parent_revision_{};
    std::uint64_t base_revision_{};
    bool closed_{};

    friend class UTXOSet;
};

class UTXOSet final : public UTXOView
{
  public:
    UTXOSet() = default;
    UTXOSet(const UTXOSet& other);
    UTXOSet& operator=(const UTXOSet& other);
    UTXOSet(UTXOSet&& other);
    UTXOSet& operator=(UTXOSet&& other) noexcept;

    [[nodiscard]] std::optional<Coin> GetCoin(const UTXOKey& key) const override;
    [[nodiscard]] bool HaveCoin(const UTXOKey& key) const noexcept override;
    [[nodiscard]] bool AddCoin(const UTXOKey& key, const Coin& coin) noexcept;
    [[nodiscard]] std::optional<Coin> SpendCoin(const UTXOKey& key) noexcept;

    // The set must outlive batches at the same address. Successful mutations
    // invalidate open batches; exclusive single-thread ownership is required.
    [[nodiscard]] std::optional<UTXOBatch> BeginBatch() noexcept;
    [[nodiscard]] ApplyTransactionResult
    ApplyTransaction(const primitives::Transaction& transaction, std::uint32_t height,
                     bool is_coinbase, const primitives::TransactionLimits& limits) noexcept;
    [[nodiscard]] UTXOError UndoTransaction(const TransactionUndo& undo) noexcept;
    [[nodiscard]] ApplyBlockResult ApplyBlock(std::span<const primitives::Transaction> transactions,
                                              std::uint32_t height,
                                              const primitives::TransactionLimits& limits) noexcept;
    [[nodiscard]] UTXOError UndoBlock(const BlockUndo& undo) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

  private:
    using Coins = std::map<UTXOKey, Coin>;

    void InvalidateBatches() noexcept;
    Coins coins_;
    std::uint64_t revision_{};

    friend class UTXOBatch;
};

} // namespace nova::chain
