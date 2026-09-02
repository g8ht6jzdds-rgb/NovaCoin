#include "explorer/explorer.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <utility>

namespace nova::explorer
{
namespace
{

[[nodiscard]] bool CheckedAdd(const primitives::Amount left, const primitives::Amount right,
                              primitives::Amount& total) noexcept
{
    if (right > 0 && left > std::numeric_limits<primitives::Amount>::max() - right) {
        return false;
    }
    total = left + right;
    return true;
}

} // namespace

NovacoindSnapshotSource::NovacoindSnapshotSource(
    const chain::ChainState& chain, const consensus::NetworkParams& parameters) noexcept
    : chain_(chain), parameters_(parameters)
{
}

std::optional<ExplorerSnapshot> NovacoindSnapshotSource::ReadSnapshot() const noexcept
{
    try {
        const auto indexes = chain_.GetActiveBlockIndexes();
        if (!indexes.has_value() || indexes->empty()) {
            return std::nullopt;
        }
        ExplorerSnapshot snapshot{parameters_.id,
                                  parameters_.difficulty.target_spacing_seconds,
                                  parameters_.pow,
                                  parameters_.block_limits,
                                  {}};
        snapshot.active_blocks.reserve(indexes->size());
        for (const auto& index : *indexes) {
            if (index.height == 0U) {
                snapshot.active_blocks.push_back(
                    ExplorerBlock{0U, parameters_.genesis_hash, parameters_.genesis_block});
                continue;
            }
            if (!index.block.has_value()) {
                return std::nullopt;
            }
            snapshot.active_blocks.push_back(ExplorerBlock{index.height, index.hash, *index.block});
        }
        return snapshot;
    } catch (...) {
        return std::nullopt;
    }
}

bool ExplorerIndex::HashLess::operator()(const crypto::Hash256& left,
                                         const crypto::Hash256& right) const noexcept
{
    return std::lexicographical_compare(left.bytes().begin(), left.bytes().end(),
                                        right.bytes().begin(), right.bytes().end());
}

std::optional<crypto::Hash160>
ExtractP2pkhAddress(const std::span<const std::uint8_t> script_pubkey) noexcept
{
    constexpr std::array<std::uint8_t, 3U> kPrefix{0x76U, 0xA9U, 0x14U};
    constexpr std::array<std::uint8_t, 2U> kSuffix{0x88U, 0xACU};
    if (script_pubkey.size() != 25U ||
        !std::equal(kPrefix.begin(), kPrefix.end(), script_pubkey.begin()) ||
        !std::equal(kSuffix.begin(), kSuffix.end(), script_pubkey.end() - 2)) {
        return std::nullopt;
    }
    crypto::Hash160 address{};
    std::copy_n(script_pubkey.begin() + 3, address.size(), address.begin());
    return address;
}

ExplorerResult ExplorerIndex::Rebuild(const ExplorerSnapshot& snapshot) noexcept
{
    if (snapshot.active_blocks.empty()) {
        return {ExplorerError::kEmptySnapshot};
    }
    if (snapshot.block_limits.transaction_limits.max_money < 0 ||
        snapshot.target_spacing_seconds == 0U || snapshot.pow_parameters.pow_limit.IsZero()) {
        return {ExplorerError::kInvalidLimits};
    }

    try {
        std::vector<crypto::Hash256> next_order;
        BlocksByHash next_blocks;
        Transactions next_transactions;
        Utxos next_utxos;
        next_order.reserve(snapshot.active_blocks.size());

        crypto::Hash256 previous_hash;
        consensus::Target256 current_target;
        for (std::size_t block_index = 0U; block_index < snapshot.active_blocks.size();
             ++block_index) {
            const auto& entry = snapshot.active_blocks.at(block_index);
            if (entry.height != block_index) {
                return {ExplorerError::kInvalidBlockHeight};
            }
            if (block_index != 0U && entry.block.header.previous_block_id != previous_hash) {
                return {ExplorerError::kInvalidBlockLink};
            }
            const auto computed_hash = primitives::ComputeBlockHash(entry.block.header);
            if (!computed_hash.has_value() || *computed_hash != entry.hash) {
                return {ExplorerError::kInvalidBlockHash};
            }
            if (primitives::CheckBlockStructure(entry.block, snapshot.block_limits) !=
                primitives::BlockStructureError::kNone) {
                return {ExplorerError::kInvalidBlockStructure};
            }
            const auto target =
                consensus::TargetFromCompact(entry.block.header.bits, snapshot.pow_parameters);
            if (target.error != consensus::TargetError::kNone || !target.target.has_value() ||
                snapshot.pow_parameters.pow_limit < *target.target) {
                return {ExplorerError::kInvalidBlockStructure};
            }
            current_target = *target.target;
            if (!next_blocks.emplace(entry.hash, entry).second) {
                return {ExplorerError::kInvalidBlockHash};
            }

            for (std::size_t transaction_index = 0U;
                 transaction_index < entry.block.transactions.size(); ++transaction_index) {
                const auto& transaction = entry.block.transactions.at(transaction_index);
                const auto transaction_id =
                    transaction.TxId(snapshot.block_limits.transaction_limits);
                if (!transaction_id.has_value()) {
                    return {ExplorerError::kInvalidBlockStructure};
                }
                if (next_transactions.contains(*transaction_id)) {
                    return {ExplorerError::kDuplicateTransaction};
                }
                const bool is_coinbase = transaction_index == 0U;
                primitives::Amount input_total = 0;
                if (!is_coinbase) {
                    for (const auto& input : transaction.inputs) {
                        const auto key = chain::UTXOKey::FromOutPoint(input.previous_output);
                        const auto previous = next_utxos.find(key);
                        if (previous == next_utxos.end()) {
                            return {ExplorerError::kMissingInput};
                        }
                        primitives::Amount updated{};
                        if (!CheckedAdd(input_total, previous->second.output.value, updated)) {
                            return {ExplorerError::kInputAmountOverflow};
                        }
                        input_total = updated;
                        next_utxos.erase(previous);
                    }
                }

                primitives::Amount output_total = 0;
                for (std::size_t output_index = 0U; output_index < transaction.outputs.size();
                     ++output_index) {
                    const auto& output = transaction.outputs.at(output_index);
                    primitives::Amount updated{};
                    if (!CheckedAdd(output_total, output.value, updated)) {
                        return {ExplorerError::kOutputAmountOverflow};
                    }
                    output_total = updated;
                    if (output_index > std::numeric_limits<std::uint32_t>::max()) {
                        return {ExplorerError::kOutputAmountOverflow};
                    }
                    const chain::UTXOKey key{*transaction_id,
                                             static_cast<std::uint32_t>(output_index)};
                    if (!next_utxos.emplace(key, chain::Coin{output, entry.height, is_coinbase})
                             .second) {
                        return {ExplorerError::kDuplicateTransaction};
                    }
                }
                std::optional<primitives::Amount> fee;
                if (!is_coinbase) {
                    if (input_total < output_total) {
                        return {ExplorerError::kNegativeFee};
                    }
                    fee = input_total - output_total;
                }
                next_transactions.emplace(
                    *transaction_id, ExplorerTransaction{*transaction_id, entry.hash, entry.height,
                                                         transaction, is_coinbase, fee});
            }
            next_order.push_back(entry.hash);
            previous_hash = entry.hash;
        }

        const auto estimated_blocks_per_day =
            static_cast<std::uint64_t>(86'400U) / snapshot.target_spacing_seconds;
        const ExplorerSummary next_summary{snapshot.network,
                                           snapshot.active_blocks.back().height,
                                           snapshot.active_blocks.back().hash,
                                           current_target,
                                           snapshot.target_spacing_seconds,
                                           estimated_blocks_per_day,
                                           next_blocks.size(),
                                           next_transactions.size(),
                                           next_utxos.size()};
        const std::unique_lock lock{mutex_};
        summary_ = next_summary;
        transaction_limits_ = snapshot.block_limits.transaction_limits;
        block_order_ = std::move(next_order);
        blocks_ = std::move(next_blocks);
        transactions_ = std::move(next_transactions);
        utxos_ = std::move(next_utxos);
        return {};
    } catch (...) {
        return {ExplorerError::kAllocationFailure};
    }
}

std::optional<ExplorerSummary> ExplorerIndex::Summary() const noexcept
{
    const std::shared_lock lock{mutex_};
    return summary_;
}

std::optional<primitives::TransactionLimits> ExplorerIndex::TransactionLimits() const noexcept
{
    const std::shared_lock lock{mutex_};
    return transaction_limits_;
}

std::vector<ExplorerBlock> ExplorerIndex::LatestBlocks(const std::size_t count) const
{
    const std::shared_lock lock{mutex_};
    std::vector<ExplorerBlock> result;
    const auto result_count = std::min(count, block_order_.size());
    result.reserve(result_count);
    for (auto iterator = block_order_.rbegin();
         iterator != block_order_.rend() && result.size() < result_count; ++iterator) {
        const auto block = blocks_.find(*iterator);
        if (block != blocks_.end()) {
            result.push_back(block->second);
        }
    }
    return result;
}

std::optional<ExplorerBlock>
ExplorerIndex::FindBlockByHeight(const std::uint32_t height) const noexcept
{
    const std::shared_lock lock{mutex_};
    if (height >= block_order_.size()) {
        return std::nullopt;
    }
    const auto found = blocks_.find(block_order_.at(height));
    if (found == blocks_.end()) {
        return std::nullopt;
    }
    try {
        return found->second;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<ExplorerBlock>
ExplorerIndex::FindBlockByHash(const crypto::Hash256& hash) const noexcept
{
    const std::shared_lock lock{mutex_};
    const auto found = blocks_.find(hash);
    if (found == blocks_.end()) {
        return std::nullopt;
    }
    try {
        return found->second;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<ExplorerTransaction>
ExplorerIndex::FindTransaction(const crypto::Hash256& transaction_id) const noexcept
{
    const std::shared_lock lock{mutex_};
    const auto found = transactions_.find(transaction_id);
    if (found == transactions_.end()) {
        return std::nullopt;
    }
    try {
        return found->second;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<ExplorerUtxo> ExplorerIndex::AllUtxos(const std::size_t maximum) const
{
    const std::shared_lock lock{mutex_};
    std::vector<ExplorerUtxo> result;
    if (maximum == 0U) {
        return result;
    }
    result.reserve(std::min(maximum, utxos_.size()));
    for (const auto& [key, coin] : utxos_) {
        result.push_back(ExplorerUtxo{key, coin, ExtractP2pkhAddress(coin.output.script_pubkey)});
        if (result.size() == maximum) {
            break;
        }
    }
    return result;
}

std::vector<ExplorerUtxo> ExplorerIndex::FindUtxosByAddress(const crypto::Hash160& address,
                                                            const std::size_t maximum) const
{
    const std::shared_lock lock{mutex_};
    std::vector<ExplorerUtxo> result;
    if (maximum == 0U) {
        return result;
    }
    result.reserve(std::min(maximum, utxos_.size()));
    for (const auto& [key, coin] : utxos_) {
        const auto output_address = ExtractP2pkhAddress(coin.output.script_pubkey);
        if (output_address.has_value() && *output_address == address) {
            result.push_back(ExplorerUtxo{key, coin, output_address});
            if (result.size() == maximum) {
                break;
            }
        }
    }
    return result;
}

} // namespace nova::explorer
