#include "chain/mempool.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <set>
#include <utility>
#include <vector>

namespace nova::chain
{
namespace
{
class ReadOnlyBatchFactory final
{
  public:
    explicit ReadOnlyBatchFactory(const UTXOView& view) noexcept : view_(view) {}
    [[nodiscard]] bool IsCurrent() const noexcept
    {
        return view_.IsCurrent();
    }
    [[nodiscard]] std::optional<UTXOBatch> BeginBatch() const noexcept
    {
        return UTXOBatch::ReadOnlyOverlay(view_);
    }

  private:
    const UTXOView& view_;
};

[[nodiscard]] bool IsNonfatalReconciliationResult(const MempoolResult& result) noexcept
{
    if (result.validation_error == BlockValidationError::kAllocationFailure ||
        result.validation_error == BlockValidationError::kSignatureHashFailure ||
        result.validation_error == BlockValidationError::kInvalidContext ||
        result.validation_error == BlockValidationError::kInvalidParameters ||
        result.validation_error == BlockValidationError::kStateApplicationFailure ||
        result.validation_error == BlockValidationError::kInvalidTransactionStructure)
        return false;
    switch (result.error) {
    case MempoolError::kNone:
    case MempoolError::kInvalidTransaction:
    case MempoolError::kDuplicateTransaction:
    case MempoolError::kMissingInput:
    case MempoolError::kMempoolDoubleSpend:
    case MempoolError::kFeeOutOfRange:
    case MempoolError::kFeeBelowMinimum:
    case MempoolError::kPoolFull:
    case MempoolError::kPoolSizeLimit:
        return true;
    case MempoolError::kSizeOverflow:
    case MempoolError::kInvalidParameters:
    case MempoolError::kStateUnavailable:
    case MempoolError::kAllocationFailure:
        return false;
    }
    return false;
}
} // namespace

bool Mempool::HashLess::operator()(const crypto::Hash256& left,
                                   const crypto::Hash256& right) const noexcept
{
    return std::lexicographical_compare(left.bytes().begin(), left.bytes().end(),
                                        right.bytes().begin(), right.bytes().end());
}

Mempool::Mempool(ConstructionKey, MempoolParams parameters)
    : identity_(std::make_shared<const std::uint8_t>(0U)), parameters_(parameters)
{
}

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
    return AcceptImpl(transaction, context, active_utxos, time_received);
}

MempoolResult Mempool::AcceptToMempool(const primitives::Transaction& transaction,
                                       const TransactionValidationContext& context,
                                       UTXOBatch& active_utxos,
                                       const std::uint64_t time_received) noexcept
{
    if (!active_utxos.IsCurrent())
        return {MempoolError::kStateUnavailable, BlockValidationError::kInvalidContext,
                std::nullopt, 0U};
    return AcceptImpl(transaction, context, active_utxos, time_received);
}

MempoolResult Mempool::AcceptToMempool(const primitives::Transaction& transaction,
                                       const TransactionValidationContext& context,
                                       const UTXOView& active_utxos,
                                       const std::uint64_t time_received) noexcept
{
    ReadOnlyBatchFactory base{active_utxos};
    if (!base.IsCurrent())
        return {MempoolError::kStateUnavailable, BlockValidationError::kInvalidContext,
                std::nullopt, 0U};
    return AcceptImpl(transaction, context, base, time_received);
}

template <typename Base>
MempoolResult Mempool::AcceptImpl(const primitives::Transaction& transaction,
                                  const TransactionValidationContext& context, Base& active_utxos,
                                  const std::uint64_t time_received) noexcept
{
    if (!CanAdvanceRevision())
        return {MempoolError::kStateUnavailable, BlockValidationError::kNone, std::nullopt, 0U};
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
    const auto fee_rate = validated.fee / static_cast<primitives::Amount>(*serialized_size);
    try {
        const auto dependencies = DependenciesFor(transaction, entries_);
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
        ++revision_;
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
    if (!CanAdvanceRevision())
        return {MempoolError::kStateUnavailable, BlockValidationError::kNone, transaction_id, 0U};
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
        ++revision_;
        return {MempoolError::kNone, BlockValidationError::kNone, transaction_id, removed_count};
    } catch (...) {
        return {MempoolError::kAllocationFailure, BlockValidationError::kAllocationFailure,
                transaction_id, 0U};
    }
}

MempoolResult
Mempool::RemoveForBlock(const std::span<const primitives::Transaction> block_transactions) noexcept
{
    if (!CanAdvanceRevision())
        return {MempoolError::kStateUnavailable, BlockValidationError::kNone, std::nullopt, 0U};
    try {
        std::set<crypto::Hash256, HashLess> confirmed;
        for (const auto& transaction : block_transactions) {
            const auto transaction_id =
                transaction.TxId(parameters_.validation.block_limits.transaction_limits);
            if (!transaction_id.has_value())
                return {MempoolError::kStateUnavailable,
                        BlockValidationError::kInvalidTransactionStructure, std::nullopt, 0U};
            confirmed.insert(*transaction_id);
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
        ++revision_;
        return {MempoolError::kNone, BlockValidationError::kNone, std::nullopt, removed_count};
    } catch (...) {
        return {MempoolError::kAllocationFailure, BlockValidationError::kAllocationFailure,
                std::nullopt, 0U};
    }
}

MempoolResult Mempool::RevalidateMempool(const TransactionValidationContext& context,
                                         UTXOSet& active_utxos) noexcept
{
    return RevalidateImpl(context, active_utxos);
}

MempoolResult Mempool::RevalidateMempool(const TransactionValidationContext& context,
                                         UTXOBatch& active_utxos) noexcept
{
    if (!active_utxos.IsCurrent())
        return {MempoolError::kStateUnavailable, BlockValidationError::kInvalidContext,
                std::nullopt, 0U};
    return RevalidateImpl(context, active_utxos);
}

MempoolResult Mempool::RevalidateMempool(const TransactionValidationContext& context,
                                         const UTXOView& active_utxos) noexcept
{
    ReadOnlyBatchFactory base{active_utxos};
    if (!base.IsCurrent())
        return {MempoolError::kStateUnavailable, BlockValidationError::kInvalidContext,
                std::nullopt, 0U};
    return RevalidateImpl(context, base);
}

template <typename Base>
MempoolResult Mempool::RevalidateImpl(const TransactionValidationContext& context,
                                      Base& active_utxos) noexcept
{
    if (!CanAdvanceRevision())
        return {MempoolError::kStateUnavailable, BlockValidationError::kNone, std::nullopt, 0U};
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
    ++revision_;
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

template <typename Base>
std::optional<UTXOBatch> Mempool::BuildOverlay(const TransactionValidationContext& context,
                                               Base& active_utxos) const noexcept
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

template <typename Base>
std::optional<Mempool::RebuiltPool>
Mempool::BuildRevalidatedPool(const TransactionValidationContext& context,
                              Base& active_utxos) const noexcept
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
                // Stored entries were admitted already. Hash/size failure or
                // mismatched identity is unavailable local state, not eviction.
                if (!transaction_id.has_value() || *transaction_id != existing->first ||
                    !serialized_size.has_value())
                    return std::nullopt;
                if (rebuilt.entries.size() >= parameters_.max_transactions ||
                    *serialized_size >
                        parameters_.max_total_serialized_size - rebuilt.total_serialized_size) {
                    iterator = remaining.erase(iterator);
                    progressed = true;
                    continue;
                }
                auto validated = ValidateTransactionAgainstUTXOSet(
                    existing->second.transaction, context, parameters_.validation, *batch);
                if (failure_point_ == FailurePoint::kRevalidationAllocation) {
                    failure_point_ = FailurePoint::kNone;
                    validated.error = BlockValidationError::kAllocationFailure;
                }
                if (validated.error == BlockValidationError::kAllocationFailure ||
                    validated.error == BlockValidationError::kSignatureHashFailure ||
                    validated.error == BlockValidationError::kInvalidContext ||
                    validated.error == BlockValidationError::kInvalidParameters ||
                    validated.error == BlockValidationError::kStateApplicationFailure)
                    return std::nullopt;
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
                if (applied.error == UTXOError::kAllocationFailure ||
                    applied.error == UTXOError::kBatchClosed)
                    return std::nullopt;
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
    if (failure_point_ == FailurePoint::kDependencyAllocation) {
        failure_point_ = FailurePoint::kNone;
        throw std::bad_alloc{};
    }
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
    if (error == BlockValidationError::kAllocationFailure)
        return MempoolError::kAllocationFailure;
    if (error == BlockValidationError::kInvalidContext)
        return MempoolError::kStateUnavailable;
    if (error == BlockValidationError::kInvalidParameters)
        return MempoolError::kInvalidParameters;
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

bool Mempool::CanAdvanceRevision() const noexcept
{
    return revision_ != std::numeric_limits<std::uint64_t>::max();
}

void Mempool::SetFailurePointForTesting(const FailurePoint point) noexcept
{
    failure_point_ = point;
}

bool Mempool::CanCommitPrepared(const PreparedState& prepared) const noexcept
{
    const auto owner = prepared.owner_.lock();
    return owner != nullptr && owner == identity_ && !prepared.consumed_ &&
           prepared.revision_ == revision_ && CanAdvanceRevision();
}

bool Mempool::CommitPrepared(PreparedState& prepared) noexcept
{
    if (!CanCommitPrepared(prepared))
        return false;
    static_assert(noexcept(entries_.swap(prepared.pool_.entries)));
    static_assert(noexcept(spends_.swap(prepared.pool_.spends)));
    entries_.swap(prepared.pool_.entries);
    spends_.swap(prepared.pool_.spends);
    total_serialized_size_ = prepared.pool_.total_serialized_size;
    ++revision_;
    prepared.consumed_ = true;
    return true;
}

std::optional<std::vector<MempoolEntry>>
Mempool::GetPreparedEntries(const PreparedState& prepared) const noexcept
{
    if (!CanCommitPrepared(prepared))
        return std::nullopt;
    try {
        std::vector<MempoolEntry> result;
        result.reserve(prepared.pool_.entries.size());
        for (const auto& [id, entry] : prepared.pool_.entries) {
            (void)id;
            result.push_back(entry);
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

Mempool::PreparationResult
Mempool::PrepareReconciliation(const std::span<const primitives::Transaction> confirmed,
                               const std::span<const primitives::Transaction> known,
                               const TransactionValidationContext& context,
                               UTXOBatch& candidate_utxos,
                               const std::uint64_t time_received) noexcept
{
    return PrepareImpl(confirmed, known, context, candidate_utxos, time_received);
}

Mempool::PreparationResult
Mempool::PrepareReconciliation(const std::span<const primitives::Transaction> confirmed,
                               const std::span<const primitives::Transaction> known,
                               const TransactionValidationContext& context,
                               const UTXOView& candidate_utxos,
                               const std::uint64_t time_received) noexcept
{
    ReadOnlyBatchFactory base{candidate_utxos};
    return PrepareImpl(confirmed, known, context, base, time_received);
}

template <typename Base>
Mempool::PreparationResult
Mempool::PrepareImpl(const std::span<const primitives::Transaction> confirmed,
                     const std::span<const primitives::Transaction> known,
                     const TransactionValidationContext& context, Base& candidate_utxos,
                     const std::uint64_t time_received) noexcept
{
    if (!CanAdvanceRevision() || !candidate_utxos.IsCurrent())
        return {{MempoolError::kStateUnavailable, BlockValidationError::kInvalidContext,
                 std::nullopt, 0U},
                nullptr};
    const auto failure = std::exchange(failure_point_, FailurePoint::kNone);
    try {
        if (failure == FailurePoint::kBeforePreparedCopy)
            throw std::bad_alloc{};
        auto prepared = std::unique_ptr<PreparedState>{new PreparedState{}};
        prepared->owner_ = identity_;
        prepared->revision_ = revision_;
        Mempool staged{ConstructionKey{}, parameters_};
        staged.entries_ = entries_;
        staged.spends_ = spends_;
        staged.total_serialized_size_ = total_serialized_size_;
        staged.failure_point_ = failure;
        auto result = staged.RemoveForBlock(confirmed);
        if (result.error != MempoolError::kNone)
            return {result, nullptr};
        result = staged.RevalidateImpl(context, candidate_utxos);
        if (result.error != MempoolError::kNone)
            return {result, nullptr};
        for (const auto& transaction : known) {
            result = staged.AcceptImpl(transaction, context, candidate_utxos, time_received);
            if (!IsNonfatalReconciliationResult(result))
                return {result, nullptr};
        }
        if (!candidate_utxos.IsCurrent())
            return {{MempoolError::kStateUnavailable, BlockValidationError::kInvalidContext,
                     std::nullopt, 0U},
                    nullptr};
        prepared->pool_.entries.swap(staged.entries_);
        prepared->pool_.spends.swap(staged.spends_);
        prepared->pool_.total_serialized_size = staged.total_serialized_size_;
        return {{MempoolError::kNone, BlockValidationError::kNone, std::nullopt, 0U},
                std::move(prepared)};
    } catch (...) {
        return {{MempoolError::kAllocationFailure, BlockValidationError::kAllocationFailure,
                 std::nullopt, 0U},
                nullptr};
    }
}

} // namespace nova::chain
