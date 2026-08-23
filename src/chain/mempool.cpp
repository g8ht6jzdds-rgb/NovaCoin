#include "chain/mempool.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace nova::chain
{

bool Mempool::HashLess::operator()(const crypto::Hash256& left,
                                   const crypto::Hash256& right) const noexcept
{
    return std::lexicographical_compare(left.bytes().begin(), left.bytes().end(),
                                        right.bytes().begin(), right.bytes().end());
}

Mempool::Mempool(ConstructionKey, MempoolParams parameters) noexcept : parameters_(parameters) {}

std::unique_ptr<Mempool> Mempool::Create(MempoolParams parameters) noexcept
{
    try {
        auto mempool = std::make_unique<Mempool>(ConstructionKey{}, parameters);
        return mempool->ParametersAreValid() ? std::move(mempool) : nullptr;
    } catch (...) {
        return nullptr;
    }
}

MempoolResult Mempool::AcceptToMempool(const primitives::Transaction& transaction,
                                       const TransactionValidationContext& context,
                                       UTXOSet& active_utxos,
                                       const std::uint64_t time_received) noexcept
{
    if (!ParametersAreValid()) {
        return {MempoolError::kInvalidParameters, BlockValidationError::kInvalidParameters,
                std::nullopt, 0U};
    }
    const auto transaction_id =
        transaction.TxId(parameters_.validation.block_limits.transaction_limits);
    if (!transaction_id.has_value()) {
        return {MempoolError::kInvalidTransaction,
                BlockValidationError::kInvalidTransactionStructure, std::nullopt, 0U};
    }
    if (entries_.contains(*transaction_id)) {
        return {MempoolError::kDuplicateTransaction, BlockValidationError::kNone, *transaction_id,
                0U};
    }
    const auto serialized_size = SerializedSizeOf(transaction);
    if (!serialized_size.has_value()) {
        return {MempoolError::kSizeOverflow, BlockValidationError::kInvalidTransactionStructure,
                *transaction_id, 0U};
    }
    if (entries_.size() >= parameters_.max_transactions) {
        return {MempoolError::kPoolFull, BlockValidationError::kNone, *transaction_id, 0U};
    }
    if (total_serialized_size_ > parameters_.max_total_serialized_size ||
        *serialized_size > parameters_.max_total_serialized_size - total_serialized_size_) {
        return {MempoolError::kPoolSizeLimit, BlockValidationError::kNone, *transaction_id, 0U};
    }
    for (const auto& input : transaction.inputs) {
        const auto key = UTXOKey::FromOutPoint(input.previous_output);
        if (spends_.contains(key)) {
            return {MempoolError::kMempoolDoubleSpend, BlockValidationError::kDoubleSpend,
                    *transaction_id, 0U};
        }
    }
    const auto overlay = BuildOverlay(context, active_utxos);
    if (!overlay.has_value()) {
        return {MempoolError::kStateUnavailable, BlockValidationError::kNone, *transaction_id, 0U};
    }
    const auto validated =
        ValidateTransactionAgainstUTXOSet(transaction, context, parameters_.validation, *overlay);
    if (validated.error != BlockValidationError::kNone) {
        return {MapValidationError(validated.error), validated.error, *transaction_id, 0U};
    }
    if (validated.fee < 0 || validated.fee > parameters_.validation.chain_parameters.max_money) {
        return {MempoolError::kFeeOutOfRange, BlockValidationError::kFeeOverflow, *transaction_id,
                0U};
    }
    if (validated.fee < parameters_.min_fee) {
        return {MempoolError::kFeeBelowMinimum, BlockValidationError::kNone, *transaction_id, 0U};
    }
    const auto dependencies = DependenciesFor(transaction, entries_);
    const auto fee_rate = validated.fee / static_cast<primitives::Amount>(*serialized_size);
    try {
        auto next_entries = entries_;
        auto next_spends = spends_;
        const auto inserted = next_entries.emplace(
            *transaction_id, MempoolEntry{*transaction_id, transaction, validated.fee,
                                          *serialized_size, fee_rate, time_received, dependencies});
        if (!inserted.second) {
            return {MempoolError::kDuplicateTransaction, BlockValidationError::kNone,
                    *transaction_id, 0U};
        }
        for (const auto& input : transaction.inputs) {
            const auto spend =
                next_spends.emplace(UTXOKey::FromOutPoint(input.previous_output), *transaction_id);
            if (!spend.second) {
                return {MempoolError::kMempoolDoubleSpend, BlockValidationError::kDoubleSpend,
                        *transaction_id, 0U};
            }
        }
        entries_.swap(next_entries);
        spends_.swap(next_spends);
        total_serialized_size_ += *serialized_size;
        return {MempoolError::kNone, BlockValidationError::kNone, *transaction_id, 0U};
    } catch (...) {
        return {MempoolError::kAllocationFailure, BlockValidationError::kAllocationFailure,
                *transaction_id, 0U};
    }
}

MempoolResult Mempool::RemoveTransaction(const crypto::Hash256& transaction_id) noexcept
{
    if (!entries_.contains(transaction_id)) {
        return {MempoolError::kNone, BlockValidationError::kNone, transaction_id, 0U};
    }
    try {
        std::set<crypto::Hash256, HashLess> removed{transaction_id};
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [entry_id, entry] : entries_) {
                if (removed.contains(entry_id)) {
                    continue;
                }
                if (std::any_of(entry.dependencies.begin(), entry.dependencies.end(),
                                [&removed](const crypto::Hash256& dependency) {
                                    return removed.contains(dependency);
                                })) {
                    removed.insert(entry_id);
                    changed = true;
                }
            }
        }
        auto next_entries = entries_;
        for (const auto& entry_id : removed) {
            next_entries.erase(entry_id);
        }
        auto next_spends = BuildSpends(next_entries);
        if (!next_spends.has_value()) {
            return {MempoolError::kAllocationFailure, BlockValidationError::kAllocationFailure,
                    transaction_id, 0U};
        }
        std::uint64_t next_total = 0U;
        for (const auto& [entry_id, entry] : next_entries) {
            static_cast<void>(entry_id);
            if (entry.serialized_size > std::numeric_limits<std::uint64_t>::max() - next_total) {
                return {MempoolError::kSizeOverflow, BlockValidationError::kNone, transaction_id,
                        0U};
            }
            next_total += entry.serialized_size;
        }
        const auto removed_count = removed.size();
        entries_.swap(next_entries);
        spends_.swap(*next_spends);
        total_serialized_size_ = next_total;
        return {MempoolError::kNone, BlockValidationError::kNone, transaction_id, removed_count};
    } catch (...) {
        return {MempoolError::kAllocationFailure, BlockValidationError::kAllocationFailure,
                transaction_id, 0U};
    }
}

MempoolResult
Mempool::RemoveForBlock(const std::span<const primitives::Transaction> block_transactions) noexcept
{
    try {
        std::set<crypto::Hash256, HashLess> confirmed;
        for (const auto& transaction : block_transactions) {
            const auto transaction_id =
                transaction.TxId(parameters_.validation.block_limits.transaction_limits);
            if (transaction_id.has_value()) {
                confirmed.insert(*transaction_id);
            }
        }
        auto next_entries = entries_;
        for (const auto& transaction_id : confirmed) {
            next_entries.erase(transaction_id);
        }
        auto next_spends = BuildSpends(next_entries);
        if (!next_spends.has_value()) {
            return {MempoolError::kAllocationFailure, BlockValidationError::kAllocationFailure,
                    std::nullopt, 0U};
        }
        std::uint64_t next_total = 0U;
        for (const auto& [transaction_id, entry] : next_entries) {
            static_cast<void>(transaction_id);
            if (entry.serialized_size > std::numeric_limits<std::uint64_t>::max() - next_total) {
                return {MempoolError::kSizeOverflow, BlockValidationError::kNone, std::nullopt, 0U};
            }
            next_total += entry.serialized_size;
        }
        const auto removed_count = entries_.size() - next_entries.size();
        entries_.swap(next_entries);
        spends_.swap(*next_spends);
        total_serialized_size_ = next_total;
        return {MempoolError::kNone, BlockValidationError::kNone, std::nullopt, removed_count};
    } catch (...) {
        return {MempoolError::kAllocationFailure, BlockValidationError::kAllocationFailure,
                std::nullopt, 0U};
    }
}

MempoolResult Mempool::RevalidateMempool(const TransactionValidationContext& context,
                                         UTXOSet& active_utxos) noexcept
{
    if (!ParametersAreValid()) {
        return {MempoolError::kInvalidParameters, BlockValidationError::kInvalidParameters,
                std::nullopt, 0U};
    }
    auto rebuilt = BuildRevalidatedPool(context, active_utxos);
    if (!rebuilt.has_value()) {
        return {MempoolError::kAllocationFailure, BlockValidationError::kAllocationFailure,
                std::nullopt, 0U};
    }
    const auto removed_count = entries_.size() - rebuilt->entries.size();
    entries_.swap(rebuilt->entries);
    spends_.swap(rebuilt->spends);
    total_serialized_size_ = rebuilt->total_serialized_size;
    return {MempoolError::kNone, BlockValidationError::kNone, std::nullopt, removed_count};
}

BlockTemplateResult Mempool::SelectForBlock(const std::uint64_t max_serialized_size) const noexcept
{
    try {
        BlockTemplateResult result;
        std::set<crypto::Hash256, HashLess> selected;
        std::uint64_t total_size = 0U;
        while (true) {
            const MempoolEntry* best = nullptr;
            for (const auto& [transaction_id, entry] : entries_) {
                if (selected.contains(transaction_id) ||
                    std::any_of(entry.dependencies.begin(), entry.dependencies.end(),
                                [&selected](const crypto::Hash256& dependency) {
                                    return !selected.contains(dependency);
                                }) ||
                    entry.serialized_size > max_serialized_size - total_size) {
                    continue;
                }
                if (best == nullptr || PreferredForBlock(entry, *best)) {
                    best = &entry;
                }
            }
            if (best == nullptr) {
                break;
            }
            result.transactions.push_back(best->transaction);
            selected.insert(best->transaction_id);
            total_size += best->serialized_size;
        }
        return result;
    } catch (...) {
        return {MempoolError::kAllocationFailure, {}};
    }
}

std::size_t Mempool::size() const noexcept
{
    return entries_.size();
}

std::uint64_t Mempool::total_serialized_size() const noexcept
{
    return total_serialized_size_;
}

std::optional<MempoolEntry> Mempool::GetEntry(const crypto::Hash256& transaction_id) const noexcept
{
    const auto found = entries_.find(transaction_id);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    try {
        return found->second;
    } catch (...) {
        return std::nullopt;
    }
}

bool Mempool::ParametersAreValid() const noexcept
{
    return parameters_.max_transactions != 0U && parameters_.max_total_serialized_size != 0U &&
           parameters_.min_fee >= 0 &&
           parameters_.min_fee <= parameters_.validation.chain_parameters.max_money &&
           consensus::CheckChainParams(parameters_.validation.chain_parameters) ==
               consensus::ChainParamsError::kNone &&
           parameters_.validation.block_limits.transaction_limits.max_money ==
               parameters_.validation.chain_parameters.max_money;
}

std::optional<UTXOBatch> Mempool::BuildOverlay(const TransactionValidationContext& context,
                                               UTXOSet& active_utxos) const noexcept
{
    auto batch = active_utxos.BeginBatch();
    if (!batch.has_value()) {
        return std::nullopt;
    }
    try {
        std::set<crypto::Hash256, HashLess> remaining;
        for (const auto& [transaction_id, entry] : entries_) {
            static_cast<void>(entry);
            remaining.insert(transaction_id);
        }
        while (!remaining.empty()) {
            bool progressed = false;
            for (auto iterator = remaining.begin(); iterator != remaining.end();) {
                const auto entry = entries_.find(*iterator);
                const auto validated = ValidateTransactionAgainstUTXOSet(
                    entry->second.transaction, context, parameters_.validation, *batch);
                if (validated.error == BlockValidationError::kMissingUtxo) {
                    ++iterator;
                    continue;
                }
                if (validated.error != BlockValidationError::kNone) {
                    return std::nullopt;
                }
                const auto applied =
                    batch->ApplyTransaction(entry->second.transaction, context.height, false,
                                            parameters_.validation.block_limits.transaction_limits);
                if (applied.error != UTXOError::kNone || !applied.undo.has_value()) {
                    return std::nullopt;
                }
                iterator = remaining.erase(iterator);
                progressed = true;
            }
            if (!progressed) {
                return std::nullopt;
            }
        }
        return batch;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Mempool::RebuiltPool>
Mempool::BuildRevalidatedPool(const TransactionValidationContext& context,
                              UTXOSet& active_utxos) const noexcept
{
    auto batch = active_utxos.BeginBatch();
    if (!batch.has_value()) {
        return std::nullopt;
    }
    try {
        RebuiltPool rebuilt;
        std::set<crypto::Hash256, HashLess> remaining;
        for (const auto& [transaction_id, entry] : entries_) {
            static_cast<void>(entry);
            remaining.insert(transaction_id);
        }
        while (!remaining.empty()) {
            bool progressed = false;
            for (auto iterator = remaining.begin(); iterator != remaining.end();) {
                const auto existing = entries_.find(*iterator);
                const auto transaction_id = existing->second.transaction.TxId(
                    parameters_.validation.block_limits.transaction_limits);
                const auto serialized_size = SerializedSizeOf(existing->second.transaction);
                if (!transaction_id.has_value() || *transaction_id != existing->first ||
                    !serialized_size.has_value() ||
                    rebuilt.entries.size() >= parameters_.max_transactions ||
                    *serialized_size >
                        parameters_.max_total_serialized_size - rebuilt.total_serialized_size) {
                    iterator = remaining.erase(iterator);
                    progressed = true;
                    continue;
                }
                const auto validated = ValidateTransactionAgainstUTXOSet(
                    existing->second.transaction, context, parameters_.validation, *batch);
                if (validated.error == BlockValidationError::kMissingUtxo) {
                    ++iterator;
                    continue;
                }
                if (validated.error != BlockValidationError::kNone || validated.fee < 0 ||
                    validated.fee < parameters_.min_fee ||
                    validated.fee > parameters_.validation.chain_parameters.max_money) {
                    iterator = remaining.erase(iterator);
                    progressed = true;
                    continue;
                }
                const auto applied =
                    batch->ApplyTransaction(existing->second.transaction, context.height, false,
                                            parameters_.validation.block_limits.transaction_limits);
                if (applied.error != UTXOError::kNone || !applied.undo.has_value()) {
                    iterator = remaining.erase(iterator);
                    progressed = true;
                    continue;
                }
                const auto dependencies =
                    DependenciesFor(existing->second.transaction, rebuilt.entries);
                const auto inserted = rebuilt.entries.emplace(
                    *transaction_id,
                    MempoolEntry{*transaction_id, existing->second.transaction, validated.fee,
                                 *serialized_size,
                                 validated.fee / static_cast<primitives::Amount>(*serialized_size),
                                 existing->second.time_received, dependencies});
                if (!inserted.second) {
                    return std::nullopt;
                }
                for (const auto& input : existing->second.transaction.inputs) {
                    const auto spend = rebuilt.spends.emplace(
                        UTXOKey::FromOutPoint(input.previous_output), *transaction_id);
                    if (!spend.second) {
                        return std::nullopt;
                    }
                }
                rebuilt.total_serialized_size += *serialized_size;
                iterator = remaining.erase(iterator);
                progressed = true;
            }
            if (!progressed) {
                break;
            }
        }
        return rebuilt;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<crypto::Hash256> Mempool::DependenciesFor(const primitives::Transaction& transaction,
                                                      const Entries& entries) const
{
    std::vector<crypto::Hash256> dependencies;
    dependencies.reserve(transaction.inputs.size());
    for (const auto& input : transaction.inputs) {
        if (entries.contains(input.previous_output.transaction_id)) {
            dependencies.push_back(input.previous_output.transaction_id);
        }
    }
    std::sort(dependencies.begin(), dependencies.end(), HashLess{});
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    return dependencies;
}

std::optional<std::uint64_t>
Mempool::SerializedSizeOf(const primitives::Transaction& transaction) noexcept
{
    const auto size = transaction.SerializedSize();
    if (!size.has_value() || *size == 0U ||
        *size > static_cast<std::uint64_t>(std::numeric_limits<primitives::Amount>::max())) {
        return std::nullopt;
    }
    return size;
}

bool Mempool::PreferredForBlock(const MempoolEntry& candidate, const MempoolEntry& current) noexcept
{
    if (candidate.fee_rate != current.fee_rate) {
        return candidate.fee_rate > current.fee_rate;
    }
    if (candidate.fee != current.fee) {
        return candidate.fee > current.fee;
    }
    return HashLess{}(candidate.transaction_id, current.transaction_id);
}

MempoolError Mempool::MapValidationError(const BlockValidationError error) noexcept
{
    if (error == BlockValidationError::kMissingUtxo) {
        return MempoolError::kMissingInput;
    }
    if (error == BlockValidationError::kDoubleSpend) {
        return MempoolError::kMempoolDoubleSpend;
    }
    if (error == BlockValidationError::kFeeOverflow ||
        error == BlockValidationError::kInputAmountOverflow) {
        return MempoolError::kFeeOutOfRange;
    }
    return MempoolError::kInvalidTransaction;
}

std::optional<Mempool::Spends> Mempool::BuildSpends(const Entries& entries) noexcept
{
    try {
        Spends spends;
        for (const auto& [transaction_id, entry] : entries) {
            for (const auto& input : entry.transaction.inputs) {
                if (!spends.emplace(UTXOKey::FromOutPoint(input.previous_output), transaction_id)
                         .second) {
                    return std::nullopt;
                }
            }
        }
        return spends;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace nova::chain
