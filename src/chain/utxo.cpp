#include "chain/utxo.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>

namespace nova::chain
{
namespace
{

constexpr std::uint64_t NextRevision(const std::uint64_t value) noexcept
{
    return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1U;
}
static_assert(NextRevision(0U) == 1U);
static_assert(NextRevision(std::numeric_limits<std::uint64_t>::max() - 1U) ==
              std::numeric_limits<std::uint64_t>::max());
static_assert(NextRevision(std::numeric_limits<std::uint64_t>::max()) ==
              std::numeric_limits<std::uint64_t>::max());

using Coins = std::map<UTXOKey, Coin>;
using Delta = UTXODelta;

[[nodiscard]] CoinReadResult ReadCoinFromDelta(const UTXOView& base, const Delta& delta,
                                               const UTXOKey& key)
{
    const auto added = delta.additions.find(key);
    if (added != delta.additions.end()) {
        return {CoinReadError::kNone, added->second};
    }
    if (delta.spends.contains(key)) {
        return {CoinReadError::kNone, std::nullopt};
    }
    return base.ReadCoin(key);
}

[[nodiscard]] bool HaveCoinInDelta(const UTXOView& base, const Delta& delta,
                                   const UTXOKey& key) noexcept
{
    return delta.additions.contains(key) || (!delta.spends.contains(key) && base.HaveCoin(key));
}

[[nodiscard]] UTXOError ApplyToDelta(const UTXOView& base, Delta& delta,
                                     const primitives::Transaction& transaction,
                                     const std::uint32_t height, const bool is_coinbase,
                                     const primitives::TransactionLimits& limits,
                                     TransactionUndo& undo)
{
    if (primitives::CheckTransactionStructure(transaction, limits) !=
        primitives::TransactionStructureError::kNone) {
        return UTXOError::kInvalidTransactionStructure;
    }
    const auto transaction_id = transaction.TxId(limits);
    if (!transaction_id.has_value()) {
        return UTXOError::kInvalidTransactionStructure;
    }
    if (is_coinbase) {
        if (transaction.inputs.size() != 1U ||
            !transaction.inputs.front().previous_output.IsNull()) {
            return UTXOError::kUnexpectedCoinbaseInput;
        }
    } else {
        for (const auto& input : transaction.inputs) {
            if (input.previous_output.IsNull()) {
                return UTXOError::kUnexpectedCoinbaseInput;
            }
            const auto key = UTXOKey::FromOutPoint(input.previous_output);
            const auto read = ReadCoinFromDelta(base, delta, key);
            if (read.error != CoinReadError::kNone)
                return UTXOError::kBatchClosed;
            const auto& coin = read.coin;
            if (!coin.has_value()) {
                return UTXOError::kMissingInput;
            }
            undo.spent_coins.push_back(SpentCoin{key, *coin});
            const auto added = delta.additions.find(key);
            if (added != delta.additions.end()) {
                delta.additions.erase(added);
            } else {
                delta.spends.emplace(key);
            }
        }
    }

    if (transaction.outputs.size() > std::numeric_limits<std::uint32_t>::max()) {
        return UTXOError::kOutputIndexOverflow;
    }
    for (std::size_t index = 0; index < transaction.outputs.size(); ++index) {
        const UTXOKey key{*transaction_id, static_cast<std::uint32_t>(index)};
        // A transaction identifier must not recreate an output already present
        // in the base set, even if another transaction in this batch spends it.
        if (base.HaveCoin(key) || delta.additions.contains(key)) {
            return UTXOError::kDuplicateCoin;
        }
        const auto inserted =
            delta.additions.emplace(key, Coin{transaction.outputs.at(index), height, is_coinbase});
        if (!inserted.second) {
            return UTXOError::kDuplicateCoin;
        }
        undo.created_coins.push_back(key);
    }
    return UTXOError::kNone;
}

[[nodiscard]] UTXOError UndoOnDelta(const UTXOView& base, Delta& delta, const TransactionUndo& undo)
{
    for (const auto& key : undo.created_coins) {
        const auto read = ReadCoinFromDelta(base, delta, key);
        if (read.error != CoinReadError::kNone)
            return UTXOError::kBatchClosed;
        if (!read.coin.has_value()) {
            return UTXOError::kUndoMissingCreatedCoin;
        }
        const auto added = delta.additions.find(key);
        if (added != delta.additions.end()) {
            delta.additions.erase(added);
        } else if (!base.HaveCoin(key) || !delta.spends.emplace(key).second) {
            return UTXOError::kUndoMissingCreatedCoin;
        }
    }
    for (const auto& spent : undo.spent_coins) {
        const auto read = ReadCoinFromDelta(base, delta, spent.key);
        if (read.error != CoinReadError::kNone)
            return UTXOError::kBatchClosed;
        if (read.coin.has_value()) {
            return UTXOError::kUndoExistingSpentCoin;
        }
        if (base.HaveCoin(spent.key)) {
            if (delta.spends.erase(spent.key) != 1U) {
                return UTXOError::kUndoExistingSpentCoin;
            }
        } else {
            const auto inserted = delta.additions.emplace(spent.key, spent.coin);
            if (!inserted.second) {
                return UTXOError::kUndoExistingSpentCoin;
            }
        }
    }
    return UTXOError::kNone;
}

} // namespace

UTXOKey UTXOKey::FromOutPoint(const primitives::OutPoint& outpoint) noexcept
{
    return UTXOKey{outpoint.transaction_id, outpoint.output_index};
}

bool operator<(const UTXOKey& left, const UTXOKey& right) noexcept
{
    if (left.transaction_id.bytes() != right.transaction_id.bytes()) {
        return std::lexicographical_compare(
            left.transaction_id.bytes().begin(), left.transaction_id.bytes().end(),
            right.transaction_id.bytes().begin(), right.transaction_id.bytes().end());
    }
    return left.output_index < right.output_index;
}

CoinReadResult UTXOView::ReadCoin(const UTXOKey& key) const
{
    if (!IsCurrent())
        return {CoinReadError::kUnavailable, std::nullopt};
    try {
        return {CoinReadError::kNone, GetCoin(key)};
    } catch (...) {
        return {CoinReadError::kUnavailable, std::nullopt};
    }
}

UTXOBatch::UTXOBatch(UTXOSet& base) : base_(base), base_set_(&base), base_revision_(base.revision_)
{
}

UTXOBatch::UTXOBatch(const UTXOView& base) : base_(base), publication_disabled_(true) {}

std::optional<UTXOBatch> UTXOBatch::ReadOnlyOverlay(const UTXOView& base) noexcept
{
    if (!base.IsCurrent())
        return std::nullopt;
    try {
        return UTXOBatch{base};
    } catch (...) {
        return std::nullopt;
    }
}

UTXOBatch::UTXOBatch(UTXOBatch& parent)
    : base_(parent), parent_(&parent), parent_revision_(parent.revision_),
      publication_disabled_(parent.publication_disabled_)
{
}

UTXOBatch::UTXOBatch(UTXOBatch&& other) noexcept(std::is_nothrow_default_constructible_v<UTXODelta>)
    : base_(other.base_), base_set_(other.base_set_), parent_(other.parent_),
      revision_(other.revision_), parent_revision_(other.parent_revision_),
      base_revision_(other.base_revision_), closed_(other.closed_),
      publication_disabled_(other.publication_disabled_), read_failed_(other.read_failed_)
{
    // Empty container construction can allocate (notably MSVC Debug).
    // It finishes before touching the source; the following swaps cannot throw.
    static_assert(std::is_nothrow_swappable_v<decltype(delta_.additions)>);
    static_assert(std::is_nothrow_swappable_v<decltype(delta_.spends)>);
    delta_.additions.swap(other.delta_.additions);
    delta_.spends.swap(other.delta_.spends);
    other.closed_ = true;
}

bool UTXOBatch::IsCurrent() const noexcept
{
    return IsOpen() && revision_ != std::numeric_limits<std::uint64_t>::max();
}

bool UTXOBatch::IsOpen() const noexcept
{
    if (closed_ || read_failed_)
        return false;
    if (parent_ != nullptr)
        return parent_->IsOpen() && parent_->revision_ == parent_revision_;
    if (publication_disabled_)
        return base_.IsCurrent();
    return base_set_ != nullptr && base_set_->revision_ == base_revision_ &&
           base_set_->revision_ != std::numeric_limits<std::uint64_t>::max();
}

ApplyTransactionResult
UTXOBatch::ApplyTransaction(const primitives::Transaction& transaction, const std::uint32_t height,
                            const bool is_coinbase,
                            const primitives::TransactionLimits& limits) noexcept
{
    if (!IsOpen() || revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return ApplyTransactionResult{UTXOError::kBatchClosed, std::nullopt};
    }
    try {
        Delta proposed{delta_.additions, delta_.spends};
        TransactionUndo undo;
        const auto error =
            ApplyToDelta(base_, proposed, transaction, height, is_coinbase, limits, undo);
        if (error != UTXOError::kNone) {
            if (error == UTXOError::kBatchClosed)
                read_failed_ = true;
            return ApplyTransactionResult{error, std::nullopt};
        }
        delta_.additions.swap(proposed.additions);
        delta_.spends.swap(proposed.spends);
        ++revision_;
        return ApplyTransactionResult{UTXOError::kNone, std::move(undo)};
    } catch (...) {
        return ApplyTransactionResult{UTXOError::kAllocationFailure, std::nullopt};
    }
}

CoinReadResult UTXOBatch::ReadCoin(const UTXOKey& key) const
{
    if (!IsCurrent())
        return {CoinReadError::kUnavailable, std::nullopt};
    try {
        auto read = ReadCoinFromDelta(base_, delta_, key);
        if (read.error != CoinReadError::kNone || !IsCurrent()) {
            read_failed_ = true;
            return {CoinReadError::kUnavailable, std::nullopt};
        }
        return read;
    } catch (...) {
        read_failed_ = true;
        return {CoinReadError::kUnavailable, std::nullopt};
    }
}

std::optional<Coin> UTXOBatch::GetCoin(const UTXOKey& key) const
{
    try {
        const auto read = ReadCoin(key);
        return read.coin;
    } catch (...) {
        read_failed_ = true;
        return std::nullopt;
    }
}

bool UTXOBatch::HaveCoin(const UTXOKey& key) const noexcept
{
    if (!IsCurrent())
        return false;
    return HaveCoinInDelta(base_, delta_, key);
}

UTXOError UTXOBatch::UndoTransaction(const TransactionUndo& undo) noexcept
{
    if (!IsOpen() || revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return UTXOError::kBatchClosed;
    }
    try {
        Delta proposed{delta_.additions, delta_.spends};
        const auto error = UndoOnDelta(base_, proposed, undo);
        if (error != UTXOError::kNone) {
            if (error == UTXOError::kBatchClosed)
                read_failed_ = true;
            return error;
        }
        delta_.additions.swap(proposed.additions);
        delta_.spends.swap(proposed.spends);
        ++revision_;
        return UTXOError::kNone;
    } catch (...) {
        return UTXOError::kAllocationFailure;
    }
}

bool UTXOBatch::Commit() noexcept
{
    if (publication_disabled_)
        return false;
    if (!IsOpen() || revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    if (parent_ != nullptr) {
        if (parent_->revision_ == std::numeric_limits<std::uint64_t>::max())
            return false;
        try {
            for (const auto& key : delta_.spends)
                if (!parent_->HaveCoin(key))
                    return false;
            for (const auto& [key, coin] : delta_.additions) {
                (void)coin;
                if (parent_->HaveCoin(key) && !delta_.spends.contains(key))
                    return false;
            }
            // Copy only the accumulated transition delta, never the base set.
            Delta proposed{parent_->delta_.additions, parent_->delta_.spends};
            for (const auto& key : delta_.spends) {
                proposed.additions.erase(key);
                if (parent_->base_.HaveCoin(key))
                    proposed.spends.emplace(key);
            }
            for (const auto& [key, coin] : delta_.additions) {
                if (!proposed.additions.emplace(key, coin).second)
                    return false;
            }
            parent_->delta_.additions.swap(proposed.additions);
            parent_->delta_.spends.swap(proposed.spends);
            ++parent_->revision_;
            closed_ = true;
            return true;
        } catch (...) {
            return false;
        }
    }
    for (const auto& key : delta_.spends) {
        if (!base_set_->coins_.contains(key)) {
            return false;
        }
    }
    for (const auto& [key, coin] : delta_.additions) {
        static_cast<void>(coin);
        if (base_set_->coins_.contains(key) && !delta_.spends.contains(key)) {
            return false;
        }
    }
    // All spends/collisions were checked above under exclusive ownership.
    // Do not construct a temporary map here: its sentinel/proxy allocation
    // can throw on MSVC even when it contains no elements. Publication uses
    // only erasure and already allocated addition nodes with equal allocators.
    static_assert(std::allocator_traits<Coins::allocator_type>::is_always_equal::value);
    static_assert(noexcept(std::declval<const UTXOKey&>() < std::declval<const UTXOKey&>()));
    for (const auto& key : delta_.spends) {
        base_set_->coins_.erase(key);
    }
    for (auto iterator = delta_.additions.begin(); iterator != delta_.additions.end();) {
        auto node = delta_.additions.extract(iterator++);
        if (!base_set_->coins_.insert(std::move(node)).inserted) {
            return false;
        }
    }
    base_set_->InvalidateBatches();
    closed_ = true;
    return true;
}

std::optional<UTXOBatch> UTXOBatch::BeginBatch() noexcept
{
    if (!IsOpen() || revision_ == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    try {
        return UTXOBatch{*this};
    } catch (...) {
        return std::nullopt;
    }
}

UTXOError UTXOBatch::UndoBlock(const BlockUndo& undo) noexcept
{
    auto child = BeginBatch();
    if (!child.has_value())
        return UTXOError::kBatchClosed;
    for (auto iterator = undo.transaction_undos.rbegin(); iterator != undo.transaction_undos.rend();
         ++iterator) {
        const auto error = child->UndoTransaction(*iterator);
        if (error != UTXOError::kNone)
            return error;
    }
    return child->Commit() ? UTXOError::kNone : UTXOError::kBatchClosed;
}

void UTXOBatch::Rollback() noexcept
{
    closed_ = true;
}

std::size_t UTXOBatch::overlay_entry_count() const noexcept
{
    return delta_.additions.size() + delta_.spends.size();
}

void UTXOSet::InvalidateBatches() noexcept
{
    revision_ = NextRevision(revision_);
}

UTXOSet::UTXOSet(const UTXOSet& other) : coins_(other.coins_) {}

UTXOSet& UTXOSet::operator=(const UTXOSet& other)
{
    if (this != &other) {
        auto proposed = other.coins_;
        coins_.swap(proposed);
        InvalidateBatches();
    }
    return *this;
}

UTXOSet::UTXOSet(UTXOSet&& other) noexcept(std::is_nothrow_default_constructible_v<Coins>)
{
    static_assert(std::is_nothrow_swappable_v<Coins>);
    coins_.swap(other.coins_);
    other.InvalidateBatches();
}

UTXOSet& UTXOSet::operator=(UTXOSet&& other) noexcept
{
    static_assert(std::is_nothrow_move_assignable_v<Coins>);
    if (this != &other) {
        coins_ = std::move(other.coins_);
        InvalidateBatches();
        other.InvalidateBatches();
    }
    return *this;
}

void UTXOSet::FailNextReadForTesting() noexcept
{
    fail_next_read_ = true;
}

CoinReadResult UTXOSet::ReadCoin(const UTXOKey& key) const
{
    if (std::exchange(fail_next_read_, false))
        return {CoinReadError::kUnavailable, std::nullopt};
    const auto found = coins_.find(key);
    if (found == coins_.end())
        return {CoinReadError::kNone, std::nullopt};
    try {
        return {CoinReadError::kNone, found->second};
    } catch (...) {
        return {CoinReadError::kUnavailable, std::nullopt};
    }
}

std::optional<Coin> UTXOSet::GetCoin(const UTXOKey& key) const
{
    const auto found = coins_.find(key);
    if (found == coins_.end()) {
        return std::nullopt;
    }
    try {
        return found->second;
    } catch (...) {
        return std::nullopt;
    }
}

bool UTXOSet::HaveCoin(const UTXOKey& key) const noexcept
{
    return coins_.contains(key);
}

bool UTXOSet::AddCoin(const UTXOKey& key, const Coin& coin) noexcept
{
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    try {
        const auto inserted = coins_.emplace(key, coin).second;
        if (inserted)
            InvalidateBatches();
        return inserted;
    } catch (...) {
        return false;
    }
}

std::optional<Coin> UTXOSet::SpendCoin(const UTXOKey& key) noexcept
{
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    const auto found = coins_.find(key);
    if (found == coins_.end()) {
        return std::nullopt;
    }
    Coin coin = std::move(found->second);
    coins_.erase(found);
    InvalidateBatches();
    return coin;
}

std::optional<UTXOBatch> UTXOSet::BeginBatch() noexcept
{
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    try {
        return UTXOBatch{*this};
    } catch (...) {
        return std::nullopt;
    }
}

ApplyTransactionResult
UTXOSet::ApplyTransaction(const primitives::Transaction& transaction, const std::uint32_t height,
                          const bool is_coinbase,
                          const primitives::TransactionLimits& limits) noexcept
{
    auto batch = BeginBatch();
    if (!batch.has_value()) {
        return ApplyTransactionResult{UTXOError::kAllocationFailure, std::nullopt};
    }
    auto result = batch->ApplyTransaction(transaction, height, is_coinbase, limits);
    if (result.error != UTXOError::kNone || !batch->Commit()) {
        if (result.error == UTXOError::kNone) {
            result.error = UTXOError::kBatchClosed;
            result.undo.reset();
        }
    }
    return result;
}

UTXOError UTXOSet::UndoTransaction(const TransactionUndo& undo) noexcept
{
    auto batch = BeginBatch();
    if (!batch.has_value()) {
        return UTXOError::kAllocationFailure;
    }
    const auto error = batch->UndoTransaction(undo);
    if (error != UTXOError::kNone) {
        return error;
    }
    return batch->Commit() ? UTXOError::kNone : UTXOError::kBatchClosed;
}

ApplyBlockResult UTXOSet::ApplyBlock(const std::span<const primitives::Transaction> transactions,
                                     const std::uint32_t height,
                                     const primitives::TransactionLimits& limits) noexcept
{
    auto batch = BeginBatch();
    if (!batch.has_value()) {
        return ApplyBlockResult{UTXOError::kAllocationFailure, std::nullopt};
    }
    try {
        BlockUndo undo;
        undo.transaction_undos.reserve(transactions.size());
        for (std::size_t index = 0; index < transactions.size(); ++index) {
            const auto& transaction = transactions[index];
            const auto is_coinbase = index == 0U && transaction.inputs.size() == 1U &&
                                     transaction.inputs.front().previous_output.IsNull();
            auto result = batch->ApplyTransaction(transaction, height, is_coinbase, limits);
            if (result.error != UTXOError::kNone || !result.undo.has_value()) {
                return ApplyBlockResult{result.error, std::nullopt};
            }
            undo.transaction_undos.push_back(std::move(*result.undo));
        }
        if (!batch->Commit()) {
            return ApplyBlockResult{UTXOError::kBatchClosed, std::nullopt};
        }
        return ApplyBlockResult{UTXOError::kNone, std::move(undo)};
    } catch (...) {
        return ApplyBlockResult{UTXOError::kAllocationFailure, std::nullopt};
    }
}

UTXOError UTXOSet::UndoBlock(const BlockUndo& undo) noexcept
{
    auto batch = BeginBatch();
    if (!batch.has_value()) {
        return UTXOError::kAllocationFailure;
    }
    for (auto iterator = undo.transaction_undos.rbegin(); iterator != undo.transaction_undos.rend();
         ++iterator) {
        const auto error = batch->UndoTransaction(*iterator);
        if (error != UTXOError::kNone) {
            return error;
        }
    }
    return batch->Commit() ? UTXOError::kNone : UTXOError::kBatchClosed;
}

std::size_t UTXOSet::size() const noexcept
{
    return coins_.size();
}

} // namespace nova::chain
