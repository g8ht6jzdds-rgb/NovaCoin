#include "chain/chainstate.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
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
           (parameters.no_retargeting || parameters.target_timespan_seconds >= 4U) &&
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

const BlockIndex* ChainState::FindIndex(const crypto::Hash256& hash) const noexcept
{
    if (erased_indexes_.contains(hash))
        return nullptr;
    const auto edited = indexes_.find(hash);
    if (edited != indexes_.end())
        return &edited->second;
    if (base_indexes_ == nullptr)
        return nullptr;
    const auto original = base_indexes_->find(hash);
    return original == base_indexes_->end() ? nullptr : &original->second;
}

BlockIndex* ChainState::EditIndex(const crypto::Hash256& hash)
{
    const auto edited = indexes_.find(hash);
    if (edited != indexes_.end())
        return &edited->second;
    const auto* original = FindIndex(hash);
    if (original == nullptr)
        return nullptr;
    return &indexes_.emplace(hash, *original).first->second;
}

void ChainState::EraseIndex(const crypto::Hash256& hash)
{
    if (base_indexes_ != nullptr && base_indexes_->contains(hash))
        erased_indexes_.insert(hash);
    indexes_.erase(hash);
}

template <typename Visitor> void ChainState::ForEachIndex(Visitor&& visitor) const
{
    if (base_indexes_ != nullptr) {
        for (const auto& [hash, index] : *base_indexes_)
            if (!erased_indexes_.contains(hash) && !indexes_.contains(hash))
                visitor(hash, index);
    }
    for (const auto& [hash, index] : indexes_)
        visitor(hash, index);
}

void ChainState::SetFailurePointForTesting(const FailurePoint point) noexcept
{
    failure_point_ = point;
}

template <typename Operation>
ChainState::PreparationResult
ChainState::PrepareOperation(Operation&& operation,
                             const std::optional<crypto::Hash256> failure_hash) noexcept
{
    if (publication_revision_ == std::numeric_limits<std::uint64_t>::max())
        return {{ChainError::kContextFailure, BlockValidationError::kInvalidContext, failure_hash},
                nullptr};
    const auto failure = std::exchange(failure_point_, FailurePoint::kNone);
    try {
        if (failure == FailurePoint::kBeforeStaging)
            throw std::bad_alloc{};
        auto prepared = std::unique_ptr<PreparedTransition>{new PreparedTransition{}};
        prepared->owner_ = identity_;
        prepared->revision_ = publication_revision_;
        auto transition_utxos = utxos_.BeginBatch();
        if (!transition_utxos.has_value())
            return {{ChainError::kAllocationFailure, BlockValidationError::kNone, failure_hash},
                    nullptr};
        prepared->utxos_.emplace(std::move(*transition_utxos));
        prepared->staged_ = std::make_unique<ChainState>(
            parameters_, utxos_, Indexes{}, active_chain_.tip(), active_chain_.height());
        auto& staged = *prepared->staged_;
        staged.base_indexes_ = &indexes_;
        staged.transition_utxos_ = &*prepared->utxos_;
        if (failure == FailurePoint::kAfterCopy)
            throw std::bad_alloc{};
        const auto result = operation(staged);
        if (result.error != ChainError::kNone)
            return {result, nullptr};
        if (failure == FailurePoint::kAfterTransition)
            throw std::bad_alloc{};
        prepared->result_ = result;
        return {result, std::move(prepared)};
    } catch (...) {
        return {{ChainError::kAllocationFailure, BlockValidationError::kNone, failure_hash},
                nullptr};
    }
}

bool ChainState::CanCommitPrepared(const PreparedTransition& prepared) const noexcept
{
    // Check owner/revision before accessing anything borrowing the live maps.
    const auto owner = prepared.owner_.lock();
    if (owner == nullptr || owner != identity_ || prepared.consumed_ ||
        prepared.revision_ != publication_revision_ ||
        publication_revision_ == std::numeric_limits<std::uint64_t>::max() ||
        prepared.staged_ == nullptr || !prepared.utxos_.has_value() ||
        !prepared.utxos_->IsCurrent())
        return false;
    const auto& staged = *prepared.staged_;
    for (const auto& hash : staged.erased_indexes_) {
        if (!indexes_.contains(hash) || staged.indexes_.contains(hash))
            return false;
    }
    return true;
}

std::optional<std::vector<BlockIndex>>
ChainState::GetPreparedActiveBlockIndexes(const PreparedTransition& prepared) const noexcept
{
    if (!CanCommitPrepared(prepared))
        return std::nullopt;
    return prepared.staged_->GetActiveBlockIndexes();
}

ChainResult ChainState::CommitPrepared(PreparedTransition& prepared) noexcept
{
    if (!CanCommitPrepared(prepared))
        return {ChainError::kContextFailure, BlockValidationError::kInvalidContext,
                prepared.result_.block_hash};
    auto& staged = *prepared.staged_;
    // Stable live addresses; no allocation, callback or I/O in publication.
    static_assert(std::allocator_traits<Indexes::allocator_type>::is_always_equal::value);
    static_assert(noexcept(HashLess{}(crypto::Hash256{}, crypto::Hash256{})));
    static_assert(std::is_nothrow_copy_assignable_v<Chain>);
    if (!prepared.utxos_->Commit())
        return {ChainError::kStateConnectFailure, BlockValidationError::kStateApplicationFailure,
                prepared.result_.block_hash};
    for (const auto& hash : staged.erased_indexes_)
        indexes_.erase(hash);
    for (auto iterator = staged.indexes_.begin(); iterator != staged.indexes_.end();) {
        const auto original = indexes_.find(iterator->first);
        // Replace the whole preallocated map node rather than swapping the
        // BlockIndex value. MSVC Debug vector move construction allocates a
        // proxy even when declared noexcept. No public API lends index refs.
        if (original != indexes_.end())
            indexes_.erase(original);
        auto node = staged.indexes_.extract(iterator++);
        indexes_.insert(std::move(node));
    }
    active_chain_ = staged.active_chain_;
    prepared.consumed_ = true;
    ++publication_revision_;
    return prepared.result_;
}

template <typename Operation>
ChainResult ChainState::ApplyAtomically(Operation&& operation,
                                        const std::optional<crypto::Hash256> failure_hash) noexcept
{
    auto prepared = PrepareOperation(std::forward<Operation>(operation), failure_hash);
    return prepared.prepared != nullptr ? CommitPrepared(*prepared.prepared) : prepared.result;
}

ChainState::PreparationResult ChainState::PrepareBlock(const primitives::Block& block) noexcept
{
    const auto hash = primitives::ComputeBlockHash(block.header);
    if (!hash.has_value())
        return {
            {ChainError::kBlockHashFailure, BlockValidationError::kBlockHashFailure, std::nullopt},
            nullptr};
    const auto parent = FindIndex(block.header.previous_block_id);
    if (parent == nullptr || !IsUsableStatus(parent->status))
        return {{ChainError::kUnknownParent, BlockValidationError::kNone, *hash}, nullptr};
    if (FindIndex(*hash) != nullptr)
        return {{ChainError::kDuplicateBlock, BlockValidationError::kNone, *hash}, nullptr};
    return PrepareOperation(
        [&block](ChainState& staged) { return staged.AcceptBlockStaged(block); }, hash);
}

ChainResult ChainState::AcceptBlock(const primitives::Block& block) noexcept
{
    auto prepared = PrepareBlock(block);
    return prepared.prepared != nullptr ? CommitPrepared(*prepared.prepared) : prepared.result;
}

ChainResult ChainState::ActivateBestChain() noexcept
{
    return ApplyAtomically([](ChainState& staged) { return staged.ActivateBestChainStaged(); },
                           std::nullopt);
}

ChainResult ChainState::RollbackAcceptedBlock(const crypto::Hash256& block_hash,
                                              const crypto::Hash256& previous_tip) noexcept
{
    return ApplyAtomically(
        [&block_hash, &previous_tip](ChainState& staged) {
            return staged.RollbackAcceptedBlockStaged(block_hash, previous_tip);
        },
        block_hash);
}

ChainResult ChainState::AcceptBlockStaged(const primitives::Block& block) noexcept
{
    const auto hash = primitives::ComputeBlockHash(block.header);
    if (!hash.has_value()) {
        return {ChainError::kBlockHashFailure, BlockValidationError::kBlockHashFailure,
                std::nullopt};
    }
    try {
        const auto* parent = FindIndex(block.header.previous_block_id);
        if (parent == nullptr || !IsUsableStatus(parent->status)) {
            return {ChainError::kUnknownParent, BlockValidationError::kNone, *hash};
        }
        if (FindIndex(*hash) != nullptr) {
            return {ChainError::kDuplicateBlock, BlockValidationError::kNone, *hash};
        }
        if (parent->height == std::numeric_limits<std::uint32_t>::max()) {
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
        auto chain_work = parent->chain_work;
        if (!chain_work.Add(*work)) {
            return {ChainError::kChainWorkOverflow, BlockValidationError::kNone, *hash};
        }
        const auto context = BuildContext(*parent, block.header.time);
        if (!context.has_value()) {
            return {ChainError::kContextFailure, BlockValidationError::kInvalidContext, *hash};
        }
        const auto expected_bits = ExpectedBits(*parent, block.header.time);
        if (!expected_bits.has_value()) {
            return {ChainError::kContextFailure, BlockValidationError::kInvalidContext, *hash};
        }
        if (block.header.bits != *expected_bits) {
            return {ChainError::kBlockValidationFailure, BlockValidationError::kUnexpectedBits,
                    *hash};
        }
        const auto preliminary = CheckBlock(block, *context, parameters_.block_validation);
        if (preliminary != BlockValidationError::kNone) {
            return {ChainError::kBlockValidationFailure, preliminary, *hash};
        }

        const auto inserted = indexes_.emplace(
            *hash, BlockIndex{*hash, block.header.previous_block_id, parent->height + 1U,
                              block.header.time, block.header.bits, *target.target, *work,
                              chain_work, BlockStatus::kPending, block, std::nullopt, 0U});
        if (!inserted.second) {
            return {ChainError::kAllocationFailure, BlockValidationError::kNone, *hash};
        }

        const auto original_tip = active_chain_.tip();
        const auto parent_hash = parent->hash;
        const auto to_parent = ReorganizeChain(parent_hash);
        if (to_parent.error != ChainError::kNone) {
            EraseIndex(*hash);
            return to_parent;
        }
        auto* candidate = EditIndex(*hash);
        if (candidate == nullptr)
            return {ChainError::kUnknownBlock, BlockValidationError::kNone, *hash};
        const auto connected = ConnectIndexedBlock(*candidate);
        if (connected.error != ChainError::kNone) {
            EraseIndex(*hash);
            const auto restored = ReorganizeChain(original_tip);
            if (restored.error != ChainError::kNone) {
                return {ChainError::kRollbackFailure, restored.validation_error, *hash};
            }
            return connected;
        }
        candidate->status = BlockStatus::kActive;
        active_chain_.SetTip(*hash, candidate->height);
        MarkActiveChain();
        const auto activation = ActivateBestChainStaged();
        if (activation.error != ChainError::kNone) {
            const auto restored = ReorganizeChain(original_tip);
            if (restored.error != ChainError::kNone) {
                return {ChainError::kRollbackFailure, restored.validation_error, *hash};
            }
            EraseIndex(*hash);
            return activation;
        }
        return {ChainError::kNone, BlockValidationError::kNone, *hash,
                active_chain_.tip() == *hash};
    } catch (...) {
        return {ChainError::kAllocationFailure, BlockValidationError::kNone, *hash};
    }
}

ChainResult ChainState::ActivateBestChainStaged() noexcept
{
    try {
        const BlockIndex* best = nullptr;
        ForEachIndex([&](const crypto::Hash256& hash, const BlockIndex& index) {
            if (!IsUsableStatus(index.status))
                return;
            if (best == nullptr || best->chain_work < index.chain_work ||
                (best->chain_work == index.chain_work && HashLess{}(hash, best->hash)))
                best = &index;
        });
        if (best == nullptr) {
            return {ChainError::kUnknownBlock, BlockValidationError::kNone, std::nullopt};
        }
        return ReorganizeChain(best->hash);
    } catch (...) {
        return {ChainError::kAllocationFailure, BlockValidationError::kNone, std::nullopt};
    }
}

ChainResult ChainState::RollbackAcceptedBlockStaged(const crypto::Hash256& block_hash,
                                                    const crypto::Hash256& previous_tip) noexcept
{
    try {
        const auto candidate = FindIndex(block_hash);
        const auto previous = FindIndex(previous_tip);
        if (candidate == nullptr || previous == nullptr) {
            return {ChainError::kUnknownBlock, BlockValidationError::kNone, block_hash};
        }
        const auto restore = ReorganizeChain(previous_tip);
        if (restore.error != ChainError::kNone) {
            return restore;
        }
        bool has_descendant = false;
        ForEachIndex([&](const crypto::Hash256&, const BlockIndex& index) {
            if (index.previous_hash == block_hash)
                has_descendant = true;
        });
        if (has_descendant)
            return {ChainError::kReorganizationFailure, BlockValidationError::kNone, block_hash};
        EraseIndex(block_hash);
        MarkActiveChain();
        return {ChainError::kNone, BlockValidationError::kNone, block_hash};
    } catch (...) {
        return {ChainError::kAllocationFailure, BlockValidationError::kNone, block_hash};
    }
}

std::optional<crypto::Hash256> ChainState::FindFork(const crypto::Hash256& left,
                                                    const crypto::Hash256& right) const noexcept
{
    const auto left_initial = FindIndex(left);
    const auto right_initial = FindIndex(right);
    if (left_initial == nullptr || right_initial == nullptr) {
        return std::nullopt;
    }
    const BlockIndex* left_index = left_initial;
    const BlockIndex* right_index = right_initial;
    while (left_index->height > right_index->height) {
        const auto parent = FindIndex(left_index->previous_hash);
        if (parent == nullptr) {
            return std::nullopt;
        }
        left_index = parent;
    }
    while (right_index->height > left_index->height) {
        const auto parent = FindIndex(right_index->previous_hash);
        if (parent == nullptr) {
            return std::nullopt;
        }
        right_index = parent;
    }
    while (left_index->hash != right_index->hash) {
        const auto left_parent = FindIndex(left_index->previous_hash);
        const auto right_parent = FindIndex(right_index->previous_hash);
        if (left_parent == nullptr || right_parent == nullptr) {
            return std::nullopt;
        }
        left_index = left_parent;
        right_index = right_parent;
    }
    return left_index->hash;
}

ChainResult ChainState::ReorganizeChain(const crypto::Hash256& new_tip) noexcept
{
    try {
        const auto target_iterator = FindIndex(new_tip);
        if (target_iterator == nullptr || !IsUsableStatus(target_iterator->status)) {
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
            const auto current = FindIndex(cursor);
            if (current == nullptr) {
                return {ChainError::kReorganizationFailure, BlockValidationError::kNone, new_tip};
            }
            cursor = current->previous_hash;
        }
        std::vector<crypto::Hash256> connect_path;
        cursor = new_tip;
        while (cursor != *fork) {
            connect_path.push_back(cursor);
            const auto current = FindIndex(cursor);
            if (current == nullptr) {
                return {ChainError::kReorganizationFailure, BlockValidationError::kNone, new_tip};
            }
            cursor = current->previous_hash;
        }
        std::reverse(connect_path.begin(), connect_path.end());

        std::vector<crypto::Hash256> disconnected;
        for (const auto& hash : disconnect_path) {
            auto* index = EditIndex(hash);
            if (index == nullptr)
                return {ChainError::kReorganizationFailure, BlockValidationError::kNone, new_tip};
            const auto error = DisconnectIndexedBlock(*index);
            if (error != ChainError::kNone) {
                for (auto iterator = disconnected.rbegin(); iterator != disconnected.rend();
                     ++iterator) {
                    auto* restore = EditIndex(*iterator);
                    if (restore == nullptr ||
                        ConnectIndexedBlock(*restore).error != ChainError::kNone) {
                        return {ChainError::kRollbackFailure, BlockValidationError::kNone, new_tip};
                    }
                }
                return {error, BlockValidationError::kNone, new_tip};
            }
            disconnected.push_back(hash);
        }

        std::vector<crypto::Hash256> connected;
        for (const auto& hash : connect_path) {
            auto* index = EditIndex(hash);
            if (index == nullptr)
                return {ChainError::kReorganizationFailure, BlockValidationError::kNone, new_tip};
            const auto result = ConnectIndexedBlock(*index);
            if (result.error != ChainError::kNone) {
                for (auto iterator = connected.rbegin(); iterator != connected.rend(); ++iterator) {
                    auto* disconnect = EditIndex(*iterator);
                    if (disconnect == nullptr ||
                        DisconnectIndexedBlock(*disconnect) != ChainError::kNone) {
                        return {ChainError::kRollbackFailure, BlockValidationError::kNone, new_tip};
                    }
                }
                for (auto iterator = disconnected.rbegin(); iterator != disconnected.rend();
                     ++iterator) {
                    auto* restore = EditIndex(*iterator);
                    if (restore == nullptr ||
                        ConnectIndexedBlock(*restore).error != ChainError::kNone) {
                        return {ChainError::kRollbackFailure, BlockValidationError::kNone, new_tip};
                    }
                }
                return result;
            }
            connected.push_back(hash);
        }
        active_chain_.SetTip(new_tip, target_iterator->height);
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
    if (base_indexes_ == nullptr)
        return indexes_.size();
    std::size_t count = 0U;
    ForEachIndex([&](const crypto::Hash256&, const BlockIndex&) { ++count; });
    return count;
}

std::optional<HeaderMetadata>
ChainState::GetHeaderMetadata(const crypto::Hash256& hash) const noexcept
{
    static_assert(std::is_nothrow_copy_constructible_v<HeaderMetadata>);
    const auto found = FindIndex(hash);
    if (found == nullptr || !IsUsableStatus(found->status)) {
        return std::nullopt;
    }
    const auto& index = *found;
    return HeaderMetadata{index.hash,           index.previous_hash, index.height,
                          index.time,           index.expected_bits, index.target,
                          index.per_block_work, index.chain_work};
}

std::optional<BlockIndex> ChainState::GetBlockIndex(const crypto::Hash256& hash) const noexcept
{
    const auto found = FindIndex(hash);
    if (found == nullptr) {
        return std::nullopt;
    }
    try {
        return *found;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::vector<BlockIndex>> ChainState::GetActiveBlockIndexes() const noexcept
{
    try {
        // Collect lightweight identifiers tip-to-anchor, then copy indexes in
        // ascending height order.  Avoid reversing BlockIndex values: those
        // contain complete blocks and reversing them performs unnecessary
        // moves of untrusted block payloads.
        std::vector<crypto::Hash256> hashes;
        hashes.reserve(static_cast<std::size_t>(active_chain_.height()) + 1U);
        auto current = FindIndex(active_chain_.tip());
        while (current != nullptr) {
            hashes.push_back(current->hash);
            const auto parent = FindIndex(current->previous_hash);
            if (parent == nullptr) {
                break;
            }
            current = parent;
        }

        std::vector<BlockIndex> active;
        active.reserve(hashes.size());
        for (auto iterator = hashes.rbegin(); iterator != hashes.rend(); ++iterator) {
            const auto index = FindIndex(*iterator);
            if (index == nullptr) {
                return std::nullopt;
            }
            active.push_back(*index);
        }
        return active;
    } catch (...) {
        return std::nullopt;
    }
}

ChainState::ChainState(ChainStateParams parameters, UTXOSet& utxos, Indexes indexes,
                       const crypto::Hash256& active_tip, const std::uint32_t active_height)
    : identity_(std::make_shared<const std::uint8_t>(0U)), parameters_(std::move(parameters)),
      utxos_(utxos), indexes_(std::move(indexes)), active_chain_(active_tip, active_height)
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
            const auto previous = FindIndex(current->previous_hash);
            if (previous == nullptr) {
                break;
            }
            current = previous;
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
        const auto parent = FindIndex(current->previous_hash);
        if (parent == nullptr || parent->height != current->height - 1U) {
            return nullptr;
        }
        current = parent;
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
            const auto previous = FindIndex(current->previous_hash);
            if (previous == nullptr || previous->height != current->height - 1U) {
                return std::nullopt;
            }
            const auto is_delayed =
                current->time > previous->time &&
                current->time - previous->time > difficulty.target_spacing_seconds * 2U;
            if (!is_delayed) {
                break;
            }
            current = previous;
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
    const auto parent = FindIndex(index.previous_hash);
    if (parent == nullptr) {
        return {ChainError::kUnknownParent, BlockValidationError::kNone, index.hash};
    }
    const auto context = BuildContext(*parent, index.block->header.time);
    if (!context.has_value()) {
        return {ChainError::kContextFailure, BlockValidationError::kInvalidContext, index.hash};
    }
    const auto expected_bits = ExpectedBits(*parent, index.block->header.time);
    if (!expected_bits.has_value() || index.block->header.bits != *expected_bits) {
        return {ChainError::kBlockValidationFailure, BlockValidationError::kUnexpectedBits,
                index.hash};
    }
    auto connected =
        transition_utxos_ != nullptr
            ? ConnectBlock(*index.block, *context, parameters_.block_validation, *transition_utxos_)
            : ConnectBlock(*index.block, *context, parameters_.block_validation, utxos_);
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
    const auto error = transition_utxos_ != nullptr
                           ? DisconnectBlock(*index.connected, *transition_utxos_)
                           : DisconnectBlock(*index.connected, utxos_);
    if (error != BlockValidationError::kNone) {
        return ChainError::kStateDisconnectFailure;
    }
    index.status = BlockStatus::kValid;
    return ChainError::kNone;
}

void ChainState::MarkActiveChain()
{
    // DisconnectIndexedBlock marks the removed suffix valid; ConnectIndexedBlock
    // marks the new suffix active. Do not rewrite/copy unchanged ancestors.
    auto current = FindIndex(active_chain_.tip());
    while (current != nullptr) {
        if (current->status != BlockStatus::kActive)
            EditIndex(current->hash)->status = BlockStatus::kActive;
        current = FindIndex(current->previous_hash);
    }
}

} // namespace nova::chain
