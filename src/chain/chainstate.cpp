#include "chain/chainstate.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace nova::chain
{
namespace
{

[[nodiscard]] bool IsUsableStatus(const BlockStatus status) noexcept
{
    return status == BlockStatus::kValid || status == BlockStatus::kActive;
}

[[nodiscard]] bool
DifficultyParametersAreSafe(const consensus::DifficultyParameters& parameters) noexcept
{
    return parameters.target_spacing_seconds != 0U && parameters.retarget_interval != 0U &&
           parameters.target_timespan_seconds != 0U &&
           parameters.target_spacing_seconds <= std::numeric_limits<std::uint32_t>::max() / 2U &&
           parameters.target_timespan_seconds <= std::numeric_limits<std::uint32_t>::max() / 4U &&
           static_cast<std::uint64_t>(parameters.target_spacing_seconds) *
                   parameters.retarget_interval ==
               parameters.target_timespan_seconds;
}

[[nodiscard]] std::optional<consensus::Target256> ScaleTarget(const consensus::Target256& target,
                                                              const std::uint32_t multiplier,
                                                              const std::uint32_t divisor) noexcept
{
    if (multiplier == 0U || divisor == 0U) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 36U> product{};
    std::uint64_t carry = 0U;
    const auto& input = target.bytes();
    for (std::size_t index = input.size(); index > 0U; --index) {
        const auto product_index = index + 3U;
        const auto value = static_cast<std::uint64_t>(input.at(index - 1U)) * multiplier + carry;
        product.at(product_index) = static_cast<std::uint8_t>(value & 0xFFU);
        carry = value >> 8U;
    }
    for (std::size_t index = 4U; index > 0U; --index) {
        product.at(index - 1U) = static_cast<std::uint8_t>(carry & 0xFFU);
        carry >>= 8U;
    }

    std::array<std::uint8_t, 36U> quotient{};
    std::uint64_t remainder = 0U;
    for (std::size_t index = 0U; index < product.size(); ++index) {
        const auto value = remainder * 256U + product.at(index);
        quotient.at(index) = static_cast<std::uint8_t>(value / divisor);
        remainder = value % divisor;
    }
    if (std::any_of(quotient.begin(), quotient.begin() + 4U,
                    [](const std::uint8_t byte) { return byte != 0U; })) {
        return std::nullopt;
    }
    consensus::Target256::Bytes result{};
    std::copy(quotient.begin() + 4U, quotient.end(), result.begin());
    return consensus::Target256{result};
}

} // namespace

ChainWork::ChainWork(Bytes bytes) noexcept : bytes_(bytes) {}

ChainWork ChainWork::FromPerBlockWork(const consensus::Target256& work) noexcept
{
    Bytes bytes{};
    std::copy(work.bytes().begin(), work.bytes().end(), bytes.end() - consensus::Target256::kSize);
    return ChainWork{bytes};
}

bool ChainWork::Add(const consensus::Target256& work) noexcept
{
    auto updated = bytes_;
    std::uint16_t carry = 0U;
    for (std::size_t index = kSize; index > 0U; --index) {
        const auto byte_index = index - 1U;
        const auto work_byte =
            byte_index >= kSize - consensus::Target256::kSize
                ? work.bytes().at(byte_index - (kSize - consensus::Target256::kSize))
                : 0U;
        const auto sum = static_cast<std::uint16_t>(updated.at(byte_index)) +
                         static_cast<std::uint16_t>(work_byte) + carry;
        updated.at(byte_index) = static_cast<std::uint8_t>(sum & std::uint16_t{0x00FFU});
        carry = static_cast<std::uint16_t>(sum >> 8U);
    }
    if (carry != 0U) {
        return false;
    }
    bytes_ = updated;
    return true;
}

const ChainWork::Bytes& ChainWork::bytes() const noexcept
{
    return bytes_;
}

std::uint32_t SystemValidationTimeSource::Now() const noexcept
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    if (seconds <= 0) {
        return 0U;
    }
    if (static_cast<std::uint64_t>(seconds) > std::numeric_limits<std::uint32_t>::max()) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(seconds);
}

bool operator<(const ChainWork& left, const ChainWork& right) noexcept
{
    return std::lexicographical_compare(left.bytes_.begin(), left.bytes_.end(),
                                        right.bytes_.begin(), right.bytes_.end());
}

Chain::Chain(const crypto::Hash256& tip, const std::uint32_t height) noexcept
    : tip_(tip), height_(height)
{
}

void Chain::SetTip(const crypto::Hash256& tip, const std::uint32_t height) noexcept
{
    tip_ = tip;
    height_ = height;
}

const crypto::Hash256& Chain::tip() const noexcept
{
    return tip_;
}

std::uint32_t Chain::height() const noexcept
{
    return height_;
}

bool ChainState::HashLess::operator()(const crypto::Hash256& left,
                                      const crypto::Hash256& right) const noexcept
{
    return std::lexicographical_compare(left.bytes().begin(), left.bytes().end(),
                                        right.bytes().begin(), right.bytes().end());
}

std::unique_ptr<ChainState> ChainState::Create(ChainStateParams parameters, ChainAnchor anchor,
                                               UTXOSet& utxos) noexcept
{
    if (parameters.median_time_past_window == 0U || parameters.validation_time_source == nullptr ||
        !DifficultyParametersAreSafe(parameters.difficulty) ||
        consensus::CheckChainParams(parameters.block_validation.chain_parameters) !=
            consensus::ChainParamsError::kNone) {
        return nullptr;
    }
    const auto expected_work = consensus::CalculateWork(anchor.target);
    const auto anchor_bits = consensus::CompactFromTarget(anchor.target);
    if (!expected_work.has_value() || !anchor_bits.has_value() ||
        *expected_work != anchor.per_block_work ||
        consensus::ValidateTarget(anchor.target,
                                  parameters.block_validation.pow_parameters.pow_limit) !=
            consensus::TargetError::kNone ||
        anchor.chain_work != ChainWork::FromPerBlockWork(anchor.per_block_work)) {
        return nullptr;
    }
    try {
        Indexes indexes;
        const auto inserted = indexes.emplace(
            anchor.hash,
            BlockIndex{anchor.hash, anchor.previous_hash, anchor.height, anchor.time, *anchor_bits,
                       anchor.target, anchor.per_block_work, anchor.chain_work,
                       BlockStatus::kActive, std::nullopt, std::nullopt, 0U});
        if (!inserted.second) {
            return nullptr;
        }
        return std::make_unique<ChainState>(std::move(parameters), utxos, std::move(indexes),
                                            anchor.hash, anchor.height);
    } catch (...) {
        return nullptr;
    }
}

ChainResult ChainState::AcceptBlock(const primitives::Block& block) noexcept
{
    const auto hash = primitives::ComputeBlockHash(block.header);
    if (!hash.has_value()) {
        return {ChainError::kBlockHashFailure, BlockValidationError::kBlockHashFailure,
                std::nullopt};
    }
    try {
        const auto parent_iterator = indexes_.find(block.header.previous_block_id);
        if (parent_iterator == indexes_.end() || !IsUsableStatus(parent_iterator->second.status)) {
            return {ChainError::kUnknownParent, BlockValidationError::kNone, *hash};
        }
        if (indexes_.contains(*hash)) {
            return {ChainError::kDuplicateBlock, BlockValidationError::kNone, *hash};
        }
        if (parent_iterator->second.height == std::numeric_limits<std::uint32_t>::max()) {
            return {ChainError::kHeightOverflow, BlockValidationError::kNone, *hash};
        }
        const auto target = consensus::TargetFromCompact(
            block.header.bits, parameters_.block_validation.pow_parameters);
        if (target.error != consensus::TargetError::kNone || !target.target.has_value()) {
            return {ChainError::kInvalidTarget, BlockValidationError::kInvalidTarget, *hash};
        }
        const auto work = consensus::CalculateWork(*target.target);
        if (!work.has_value()) {
            return {ChainError::kWorkCalculationFailure, BlockValidationError::kNone, *hash};
        }
        auto chain_work = parent_iterator->second.chain_work;
        if (!chain_work.Add(*work)) {
            return {ChainError::kChainWorkOverflow, BlockValidationError::kNone, *hash};
        }
        const auto context = BuildContext(parent_iterator->second, block.header.time);
        if (!context.has_value()) {
            return {ChainError::kContextFailure, BlockValidationError::kInvalidContext, *hash};
        }
        const auto expected_bits = ExpectedBits(parent_iterator->second, block.header.time);
        if (!expected_bits.has_value() || block.header.bits != *expected_bits) {
            return {ChainError::kBlockValidationFailure, BlockValidationError::kUnexpectedBits,
                    *hash};
        }
        const auto preliminary = CheckBlock(block, *context, parameters_.block_validation);
        if (preliminary != BlockValidationError::kNone) {
            return {ChainError::kBlockValidationFailure, preliminary, *hash};
        }

        const auto inserted = indexes_.emplace(
            *hash,
            BlockIndex{*hash, block.header.previous_block_id, parent_iterator->second.height + 1U,
                       block.header.time, block.header.bits, *target.target, *work, chain_work,
                       BlockStatus::kPending, block, std::nullopt, 0U});
        if (!inserted.second) {
            return {ChainError::kAllocationFailure, BlockValidationError::kNone, *hash};
        }

        const auto original_tip = active_chain_.tip();
        const auto parent_hash = parent_iterator->first;
        const auto to_parent = ReorganizeChain(parent_hash);
        if (to_parent.error != ChainError::kNone) {
            indexes_.erase(*hash);
            return to_parent;
        }
        auto candidate_iterator = indexes_.find(*hash);
        const auto connected = ConnectIndexedBlock(candidate_iterator->second);
        if (connected.error != ChainError::kNone) {
            indexes_.erase(candidate_iterator);
            const auto restored = ReorganizeChain(original_tip);
            if (restored.error != ChainError::kNone) {
                return {ChainError::kRollbackFailure, restored.validation_error, *hash};
            }
            return connected;
        }
        candidate_iterator->second.status = BlockStatus::kActive;
        active_chain_.SetTip(*hash, candidate_iterator->second.height);
        MarkActiveChain();
        const auto activation = ActivateBestChain();
        if (activation.error != ChainError::kNone) {
            const auto restored = ReorganizeChain(original_tip);
            if (restored.error != ChainError::kNone) {
                return {ChainError::kRollbackFailure, restored.validation_error, *hash};
            }
            indexes_.erase(*hash);
            return activation;
        }
        return {ChainError::kNone, BlockValidationError::kNone, *hash,
                active_chain_.tip() == *hash};
    } catch (...) {
        return {ChainError::kAllocationFailure, BlockValidationError::kNone, *hash};
    }
}

ChainResult ChainState::ActivateBestChain() noexcept
{
    try {
        const BlockIndex* best = nullptr;
        for (const auto& [hash, index] : indexes_) {
            if (!IsUsableStatus(index.status)) {
                continue;
            }
            if (best == nullptr || best->chain_work < index.chain_work ||
                (best->chain_work == index.chain_work && HashLess{}(hash, best->hash))) {
                best = &index;
            }
        }
        if (best == nullptr) {
            return {ChainError::kUnknownBlock, BlockValidationError::kNone, std::nullopt};
        }
        return ReorganizeChain(best->hash);
    } catch (...) {
        return {ChainError::kAllocationFailure, BlockValidationError::kNone, std::nullopt};
    }
}

ChainResult ChainState::RollbackAcceptedBlock(const crypto::Hash256& block_hash,
                                              const crypto::Hash256& previous_tip) noexcept
{
    try {
        const auto candidate = indexes_.find(block_hash);
        const auto previous = indexes_.find(previous_tip);
        if (candidate == indexes_.end() || previous == indexes_.end()) {
            return {ChainError::kUnknownBlock, BlockValidationError::kNone, block_hash};
        }
        const auto restore = ReorganizeChain(previous_tip);
        if (restore.error != ChainError::kNone) {
            return restore;
        }
        for (const auto& [hash, index] : indexes_) {
            if (index.previous_hash == block_hash) {
                return {ChainError::kReorganizationFailure, BlockValidationError::kNone,
                        block_hash};
            }
        }
        indexes_.erase(candidate);
        MarkActiveChain();
        return {ChainError::kNone, BlockValidationError::kNone, block_hash};
    } catch (...) {
        return {ChainError::kAllocationFailure, BlockValidationError::kNone, block_hash};
    }
}

std::optional<crypto::Hash256> ChainState::FindFork(const crypto::Hash256& left,
                                                    const crypto::Hash256& right) const noexcept
{
    const auto left_initial = indexes_.find(left);
    const auto right_initial = indexes_.find(right);
    if (left_initial == indexes_.end() || right_initial == indexes_.end()) {
        return std::nullopt;
    }
    const BlockIndex* left_index = &left_initial->second;
    const BlockIndex* right_index = &right_initial->second;
    while (left_index->height > right_index->height) {
        const auto parent = indexes_.find(left_index->previous_hash);
        if (parent == indexes_.end()) {
            return std::nullopt;
        }
        left_index = &parent->second;
    }
    while (right_index->height > left_index->height) {
        const auto parent = indexes_.find(right_index->previous_hash);
        if (parent == indexes_.end()) {
            return std::nullopt;
        }
        right_index = &parent->second;
    }
    while (left_index->hash != right_index->hash) {
        const auto left_parent = indexes_.find(left_index->previous_hash);
        const auto right_parent = indexes_.find(right_index->previous_hash);
        if (left_parent == indexes_.end() || right_parent == indexes_.end()) {
            return std::nullopt;
        }
        left_index = &left_parent->second;
        right_index = &right_parent->second;
    }
    return left_index->hash;
}

ChainResult ChainState::ReorganizeChain(const crypto::Hash256& new_tip) noexcept
{
    try {
        const auto target_iterator = indexes_.find(new_tip);
        if (target_iterator == indexes_.end() || !IsUsableStatus(target_iterator->second.status)) {
            return {ChainError::kUnknownBlock, BlockValidationError::kNone, new_tip};
        }
        if (new_tip == active_chain_.tip()) {
            return {ChainError::kNone, BlockValidationError::kNone, new_tip};
        }
        const auto fork = FindFork(active_chain_.tip(), new_tip);
        if (!fork.has_value()) {
            return {ChainError::kReorganizationFailure, BlockValidationError::kNone, new_tip};
        }

        std::vector<crypto::Hash256> disconnect_path;
        auto cursor = active_chain_.tip();
        while (cursor != *fork) {
            disconnect_path.push_back(cursor);
            const auto current = indexes_.find(cursor);
            if (current == indexes_.end()) {
                return {ChainError::kReorganizationFailure, BlockValidationError::kNone, new_tip};
            }
            cursor = current->second.previous_hash;
        }
        std::vector<crypto::Hash256> connect_path;
        cursor = new_tip;
        while (cursor != *fork) {
            connect_path.push_back(cursor);
            const auto current = indexes_.find(cursor);
            if (current == indexes_.end()) {
                return {ChainError::kReorganizationFailure, BlockValidationError::kNone, new_tip};
            }
            cursor = current->second.previous_hash;
        }
        std::reverse(connect_path.begin(), connect_path.end());

        std::vector<crypto::Hash256> disconnected;
        for (const auto& hash : disconnect_path) {
            auto index = indexes_.find(hash);
            const auto error = DisconnectIndexedBlock(index->second);
            if (error != ChainError::kNone) {
                for (auto iterator = disconnected.rbegin(); iterator != disconnected.rend();
                     ++iterator) {
                    auto restore = indexes_.find(*iterator);
                    if (ConnectIndexedBlock(restore->second).error != ChainError::kNone) {
                        return {ChainError::kRollbackFailure, BlockValidationError::kNone, new_tip};
                    }
                }
                return {error, BlockValidationError::kNone, new_tip};
            }
            disconnected.push_back(hash);
        }

        std::vector<crypto::Hash256> connected;
        for (const auto& hash : connect_path) {
            auto index = indexes_.find(hash);
            const auto result = ConnectIndexedBlock(index->second);
            if (result.error != ChainError::kNone) {
                for (auto iterator = connected.rbegin(); iterator != connected.rend(); ++iterator) {
                    auto disconnect = indexes_.find(*iterator);
                    if (DisconnectIndexedBlock(disconnect->second) != ChainError::kNone) {
                        return {ChainError::kRollbackFailure, BlockValidationError::kNone, new_tip};
                    }
                }
                for (auto iterator = disconnected.rbegin(); iterator != disconnected.rend();
                     ++iterator) {
                    auto restore = indexes_.find(*iterator);
                    if (ConnectIndexedBlock(restore->second).error != ChainError::kNone) {
                        return {ChainError::kRollbackFailure, BlockValidationError::kNone, new_tip};
                    }
                }
                return result;
            }
            connected.push_back(hash);
        }
        active_chain_.SetTip(new_tip, target_iterator->second.height);
        MarkActiveChain();
        return {ChainError::kNone, BlockValidationError::kNone, new_tip};
    } catch (...) {
        return {ChainError::kAllocationFailure, BlockValidationError::kNone, new_tip};
    }
}

const crypto::Hash256& ChainState::active_tip() const noexcept
{
    return active_chain_.tip();
}

std::uint32_t ChainState::active_height() const noexcept
{
    return active_chain_.height();
}

std::size_t ChainState::known_block_count() const noexcept
{
    return indexes_.size();
}

std::optional<BlockIndex> ChainState::GetBlockIndex(const crypto::Hash256& hash) const noexcept
{
    const auto found = indexes_.find(hash);
    if (found == indexes_.end()) {
        return std::nullopt;
    }
    try {
        return found->second;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::vector<BlockIndex>> ChainState::GetActiveBlockIndexes() const noexcept
{
    try {
        std::vector<BlockIndex> active;
        active.reserve(static_cast<std::size_t>(active_chain_.height()) + 1U);
        auto current = indexes_.find(active_chain_.tip());
        while (current != indexes_.end()) {
            active.push_back(current->second);
            const auto parent = indexes_.find(current->second.previous_hash);
            if (parent == indexes_.end()) {
                break;
            }
            current = parent;
        }
        std::reverse(active.begin(), active.end());
        return active;
    } catch (...) {
        return std::nullopt;
    }
}

ChainState::ChainState(ChainStateParams parameters, UTXOSet& utxos, Indexes indexes,
                       const crypto::Hash256& active_tip,
                       const std::uint32_t active_height) noexcept
    : parameters_(std::move(parameters)), utxos_(utxos), indexes_(std::move(indexes)),
      active_chain_(active_tip, active_height)
{
}

std::optional<BlockValidationContext>
ChainState::BuildContext(const BlockIndex& parent, const std::uint32_t candidate_time) const
{
    static_cast<void>(candidate_time);
    if (parent.height == std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    try {
        std::vector<std::uint32_t> times;
        times.reserve(parameters_.median_time_past_window);
        const BlockIndex* current = &parent;
        for (std::uint32_t count = 0U; count < parameters_.median_time_past_window; ++count) {
            times.push_back(current->time);
            const auto previous = indexes_.find(current->previous_hash);
            if (previous == indexes_.end()) {
                break;
            }
            current = &previous->second;
        }
        if (times.empty()) {
            return std::nullopt;
        }
        std::sort(times.begin(), times.end());
        return BlockValidationContext{parent.hash, parent.height + 1U,
                                      times.at((times.size() - 1U) / 2U),
                                      parameters_.validation_time_source->Now()};
    } catch (...) {
        return std::nullopt;
    }
}

const BlockIndex* ChainState::Ancestor(const BlockIndex& start,
                                       const std::uint32_t height) const noexcept
{
    const BlockIndex* current = &start;
    while (current->height > height) {
        const auto parent = indexes_.find(current->previous_hash);
        if (parent == indexes_.end()) {
            return nullptr;
        }
        current = &parent->second;
    }
    return current->height == height ? current : nullptr;
}

std::optional<std::uint32_t>
ChainState::ExpectedBits(const BlockIndex& parent,
                         const std::uint32_t candidate_time) const noexcept
{
    const auto& difficulty = parameters_.difficulty;
    if (!DifficultyParametersAreSafe(difficulty) ||
        parent.height == std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    const auto child_height = parent.height + 1U;
    const auto pow_limit = parameters_.block_validation.pow_parameters.pow_limit_compact;
    if (difficulty.no_retargeting) {
        return parent.expected_bits;
    }
    if (child_height % difficulty.retarget_interval != 0U) {
        const auto delayed = candidate_time > parent.time &&
                             candidate_time - parent.time > difficulty.target_spacing_seconds * 2U;
        if (difficulty.allow_min_difficulty_blocks && delayed) {
            return pow_limit;
        }
        if (!difficulty.allow_min_difficulty_blocks) {
            return parent.expected_bits;
        }
        const BlockIndex* current = &parent;
        while (current->height % difficulty.retarget_interval != 0U &&
               current->expected_bits == pow_limit) {
            const auto previous = indexes_.find(current->previous_hash);
            if (previous == indexes_.end()) {
                break;
            }
            const auto is_delayed =
                current->time > previous->second.time &&
                current->time - previous->second.time > difficulty.target_spacing_seconds * 2U;
            if (!is_delayed) {
                break;
            }
            current = &previous->second;
        }
        return current->expected_bits;
    }

    if (parent.height + 1U < difficulty.retarget_interval) {
        return std::nullopt;
    }
    const auto first_height = parent.height + 1U - difficulty.retarget_interval;
    const auto first = Ancestor(parent, first_height);
    if (first == nullptr) {
        return std::nullopt;
    }
    const auto old_target = consensus::TargetFromCompact(
        parent.expected_bits, parameters_.block_validation.pow_parameters);
    if (!old_target.target.has_value()) {
        return std::nullopt;
    }
    const auto elapsed = parent.time > first->time ? parent.time - first->time : 0U;
    const auto minimum = difficulty.target_timespan_seconds / 4U;
    const auto maximum = difficulty.target_timespan_seconds * 4U;
    const auto actual = std::clamp(elapsed, minimum, maximum);
    const auto scaled = ScaleTarget(*old_target.target, actual, difficulty.target_timespan_seconds);
    const auto target =
        !scaled.has_value() || parameters_.block_validation.pow_parameters.pow_limit < *scaled
            ? parameters_.block_validation.pow_parameters.pow_limit
            : *scaled;
    return consensus::CompactFromTarget(target);
}

ChainResult ChainState::ConnectIndexedBlock(BlockIndex& index) noexcept
{
    if (!index.block.has_value()) {
        return {ChainError::kStateConnectFailure, BlockValidationError::kNone, index.hash};
    }
    const auto parent = indexes_.find(index.previous_hash);
    if (parent == indexes_.end()) {
        return {ChainError::kUnknownParent, BlockValidationError::kNone, index.hash};
    }
    const auto context = BuildContext(parent->second, index.block->header.time);
    if (!context.has_value()) {
        return {ChainError::kContextFailure, BlockValidationError::kInvalidContext, index.hash};
    }
    const auto expected_bits = ExpectedBits(parent->second, index.block->header.time);
    if (!expected_bits.has_value() || index.block->header.bits != *expected_bits) {
        return {ChainError::kBlockValidationFailure, BlockValidationError::kUnexpectedBits,
                index.hash};
    }
    auto connected = ConnectBlock(*index.block, *context, parameters_.block_validation, utxos_);
    if (connected.error != BlockValidationError::kNone || !connected.connected.has_value()) {
        return {ChainError::kBlockValidationFailure, connected.error, index.hash};
    }
    index.connected = std::move(*connected.connected);
    index.status = BlockStatus::kActive;
    return {ChainError::kNone, BlockValidationError::kNone, index.hash};
}

ChainError ChainState::DisconnectIndexedBlock(BlockIndex& index) noexcept
{
    if (!index.connected.has_value()) {
        return ChainError::kStateDisconnectFailure;
    }
    const auto error = DisconnectBlock(*index.connected, utxos_);
    if (error != BlockValidationError::kNone) {
        return ChainError::kStateDisconnectFailure;
    }
    index.status = BlockStatus::kValid;
    return ChainError::kNone;
}

void ChainState::MarkActiveChain() noexcept
{
    for (auto& [hash, index] : indexes_) {
        static_cast<void>(hash);
        if (index.status != BlockStatus::kPending && index.status != BlockStatus::kInvalid) {
            index.status = BlockStatus::kValid;
        }
    }
    auto current = indexes_.find(active_chain_.tip());
    while (current != indexes_.end()) {
        current->second.status = BlockStatus::kActive;
        const auto parent = indexes_.find(current->second.previous_hash);
        if (parent == indexes_.end()) {
            break;
        }
        current = parent;
    }
}

} // namespace nova::chain
