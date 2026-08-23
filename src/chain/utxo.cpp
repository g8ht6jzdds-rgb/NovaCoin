#include "chain/utxo.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace nova::chain
{
namespace
{

using Coins = std::map<UTXOKey, Coin>;
using Delta = UTXODelta;

[[nodiscard]] std::optional<Coin> GetCoinFromDelta(const Coins& base, const Delta& delta,
                                                   const UTXOKey& key)
{
    const auto added = delta.additions.find(key);
    if (added != delta.additions.end()) {
        return added->second;
    }
    if (delta.spends.contains(key)) {
        return std::nullopt;
    }
    const auto found = base.find(key);
    return found == base.end() ? std::nullopt : std::optional<Coin>{found->second};
}

[[nodiscard]] bool HaveCoinInDelta(const Coins& base, const Delta& delta,
                                   const UTXOKey& key) noexcept
{
    return delta.additions.contains(key) || (!delta.spends.contains(key) && base.contains(key));
}

[[nodiscard]] UTXOError ApplyToDelta(const Coins& base, Delta& delta,
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
            const auto coin = GetCoinFromDelta(base, delta, key);
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
        if (base.contains(key) || delta.additions.contains(key)) {
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

[[nodiscard]] UTXOError UndoOnDelta(const Coins& base, Delta& delta, const TransactionUndo& undo)
{
    for (const auto& key : undo.created_coins) {
        if (!GetCoinFromDelta(base, delta, key).has_value()) {
            return UTXOError::kUndoMissingCreatedCoin;
        }
        const auto added = delta.additions.find(key);
        if (added != delta.additions.end()) {
            delta.additions.erase(added);
        } else if (!base.contains(key) || !delta.spends.emplace(key).second) {
            return UTXOError::kUndoMissingCreatedCoin;
        }
    }
    for (const auto& spent : undo.spent_coins) {
        if (GetCoinFromDelta(base, delta, spent.key).has_value()) {
            return UTXOError::kUndoExistingSpentCoin;
        }
        if (base.contains(spent.key)) {
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

UTXOBatch::UTXOBatch(UTXOSet& base) noexcept : base_(base) {}

ApplyTransactionResult
UTXOBatch::ApplyTransaction(const primitives::Transaction& transaction, const std::uint32_t height,
                            const bool is_coinbase,
                            const primitives::TransactionLimits& limits) noexcept
{
    if (closed_) {
        return ApplyTransactionResult{UTXOError::kBatchClosed, std::nullopt};
    }
    try {
        Delta proposed{delta_.additions, delta_.spends};
        TransactionUndo undo;
        const auto error =
            ApplyToDelta(base_.coins_, proposed, transaction, height, is_coinbase, limits, undo);
        if (error != UTXOError::kNone) {
            return ApplyTransactionResult{error, std::nullopt};
        }
        delta_.additions.swap(proposed.additions);
        delta_.spends.swap(proposed.spends);
        return ApplyTransactionResult{UTXOError::kNone, std::move(undo)};
    } catch (...) {
        return ApplyTransactionResult{UTXOError::kAllocationFailure, std::nullopt};
    }
}

std::optional<Coin> UTXOBatch::GetCoin(const UTXOKey& key) const
{
    try {
        return GetCoinFromDelta(base_.coins_, delta_, key);
    } catch (...) {
        return std::nullopt;
    }
}

bool UTXOBatch::HaveCoin(const UTXOKey& key) const noexcept
{
    return HaveCoinInDelta(base_.coins_, delta_, key);
}

UTXOError UTXOBatch::UndoTransaction(const TransactionUndo& undo) noexcept
{
    if (closed_) {
        return UTXOError::kBatchClosed;
    }
    try {
        Delta proposed{delta_.additions, delta_.spends};
        const auto error = UndoOnDelta(base_.coins_, proposed, undo);
        if (error != UTXOError::kNone) {
            return error;
        }
        delta_.additions.swap(proposed.additions);
        delta_.spends.swap(proposed.spends);
        return UTXOError::kNone;
    } catch (...) {
        return UTXOError::kAllocationFailure;
    }
}

bool UTXOBatch::Commit() noexcept
{
    if (closed_) {
        return false;
    }
    for (const auto& key : delta_.spends) {
        if (!base_.coins_.contains(key)) {
            return false;
        }
    }
    for (const auto& [key, coin] : delta_.additions) {
        static_cast<void>(coin);
        if (base_.coins_.contains(key) && !delta_.spends.contains(key)) {
            return false;
        }
    }
    Coins removed;
    for (const auto& key : delta_.spends) {
        auto node = base_.coins_.extract(key);
        if (node.empty() || !removed.insert(std::move(node)).inserted) {
            return false;
        }
    }
    for (auto iterator = delta_.additions.begin(); iterator != delta_.additions.end();) {
        auto node = delta_.additions.extract(iterator++);
        if (!base_.coins_.insert(std::move(node)).inserted) {
            return false;
        }
    }
    closed_ = true;
    return true;
}

void UTXOBatch::Rollback() noexcept
{
    closed_ = true;
}

std::size_t UTXOBatch::overlay_entry_count() const noexcept
{
    return delta_.additions.size() + delta_.spends.size();
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
    try {
        return coins_.emplace(key, coin).second;
    } catch (...) {
        return false;
    }
}

std::optional<Coin> UTXOSet::SpendCoin(const UTXOKey& key) noexcept
{
    const auto found = coins_.find(key);
    if (found == coins_.end()) {
        return std::nullopt;
    }
    Coin coin = std::move(found->second);
    coins_.erase(found);
    return coin;
}

std::optional<UTXOBatch> UTXOSet::BeginBatch() noexcept
{
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
