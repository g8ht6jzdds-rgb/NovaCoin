#include "node/sync.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace nova::node
{
namespace
{

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
    if (multiplier == 0U || divisor == 0U)
        return std::nullopt;
    std::array<std::uint8_t, 36U> product{};
    std::uint64_t carry{};
    for (std::size_t index = target.bytes().size(); index > 0U; --index) {
        const auto value =
            static_cast<std::uint64_t>(target.bytes().at(index - 1U)) * multiplier + carry;
        product.at(index + 3U) = static_cast<std::uint8_t>(value & 0xFFU);
        carry = value >> 8U;
    }
    for (std::size_t index = 4U; index > 0U; --index) {
        product.at(index - 1U) = static_cast<std::uint8_t>(carry & 0xFFU);
        carry >>= 8U;
    }
    std::array<std::uint8_t, 36U> quotient{};
    std::uint64_t remainder{};
    for (std::size_t index = 0U; index < product.size(); ++index) {
        const auto value = remainder * 256U + product.at(index);
        quotient.at(index) = static_cast<std::uint8_t>(value / divisor);
        remainder = value % divisor;
    }
    if (std::any_of(quotient.begin(), quotient.begin() + 4U,
                    [](const std::uint8_t byte) { return byte != 0U; }))
        return std::nullopt;
    consensus::Target256::Bytes result{};
    std::copy(quotient.begin() + 4U, quotient.end(), result.begin());
    return consensus::Target256{result};
}

} // namespace

Synchronizer::Synchronizer(SyncParams parameters, chain::ChainState& chain) noexcept
    : parameters_(parameters), chain_(chain), best_header_tip_(chain.active_tip())
{
    static_cast<void>(RefreshValidatedHeaders());
}

SyncError Synchronizer::RefreshValidatedHeaders() noexcept
{
    const auto tip = chain_.active_tip();
    if (validated_tip_ == tip) {
        return SyncError::kNone;
    }
    try {
        // Merge only the missing validated suffix. Do not discard header-only
        // branches or outstanding bodies learned from other peers.
        std::vector<HeaderIndex> missing;
        auto cursor = tip;
        while (!headers_.contains(cursor)) {
            const auto index = chain_.GetBlockIndex(cursor);
            if (!index.has_value()) {
                return SyncError::kChainStateUnavailable;
            }
            missing.push_back({index->hash, index->previous_hash, index->height, index->time,
                               index->expected_bits, index->target, index->per_block_work,
                               index->chain_work});
            if (index->height == 0U) {
                break;
            }
            cursor = index->previous_hash;
        }
        for (auto index = missing.rbegin(); index != missing.rend(); ++index) {
            headers_.emplace(index->hash, *index);
        }
        const auto active = headers_.find(tip);
        if (active == headers_.end()) {
            return SyncError::kChainStateUnavailable;
        }
        const auto best = headers_.find(best_header_tip_);
        if (best == headers_.end() || best->second.chain_work < active->second.chain_work ||
            (best->second.chain_work == active->second.chain_work &&
             HashLess{}(tip, best_header_tip_))) {
            best_header_tip_ = tip;
        }
        validated_tip_ = tip;
        return SyncError::kNone;
    } catch (...) {
        return SyncError::kAllocationFailure;
    }
}

SyncResult Synchronizer::Start() noexcept
{
    const auto refreshed = RefreshValidatedHeaders();
    if (refreshed != SyncError::kNone) {
        return {refreshed, {}, {}};
    }
    try {
        // The locator describes validated bodies, not an old side branch or a
        // header-only tip whose body has not arrived yet.
        return {SyncError::kNone,
                {},
                {{net::Command::kGetHeaders, net::GetHeadersMessage{parameters_.protocol_version,
                                                                    {chain_.active_tip()},
                                                                    crypto::Hash256{}}}}};
    } catch (...) {
        return {SyncError::kAllocationFailure, {}, {}};
    }
}

SyncResult Synchronizer::Handle(const net::FramedMessage& message) noexcept
{
    if (message.command == net::Command::kHeaders) {
        const auto* headers = std::get_if<net::HeadersMessage>(&message.message);
        return headers == nullptr ? SyncResult{SyncError::kUnexpectedMessage, {}, {}}
                                  : AcceptHeaders(*headers);
    }
    if (message.command == net::Command::kBlock) {
        const auto* block = std::get_if<primitives::Block>(&message.message);
        return block == nullptr ? SyncResult{SyncError::kUnexpectedMessage, {}, {}}
                                : AcceptBlock(*block);
    }
    return {SyncError::kUnexpectedMessage, {}, {}};
}

const crypto::Hash256& Synchronizer::best_header_tip() const noexcept
{
    return best_header_tip_;
}
std::size_t Synchronizer::header_count() const noexcept
{
    return headers_.size();
}

bool Synchronizer::HashLess::operator()(const crypto::Hash256& left,
                                        const crypto::Hash256& right) const noexcept
{
    return std::lexicographical_compare(left.bytes().begin(), left.bytes().end(),
                                        right.bytes().begin(), right.bytes().end());
}

SyncResult Synchronizer::AcceptHeaders(const net::HeadersMessage& headers) noexcept
{
    if (headers.headers.size() > parameters_.max_headers_per_response ||
        !DifficultyParametersAreSafe(parameters_.difficulty)) {
        return {SyncError::kInvalidParameters, {}, {}};
    }
    const auto refreshed = RefreshValidatedHeaders();
    if (refreshed != SyncError::kNone) {
        return {refreshed, {}, {}};
    }
    try {
        struct Batch final {
            decltype(headers_)& indexes;
            crypto::Hash256& best;
            crypto::Hash256 previous;
            std::vector<crypto::Hash256> added;
            bool committed{};
            ~Batch()
            {
                if (!committed) {
                    for (const auto& hash : added) {
                        indexes.erase(hash);
                    }
                    best = previous;
                }
            }
        } batch{headers_, best_header_tip_, best_header_tip_, {}, false};
        batch.added.reserve(headers.headers.size());
        std::vector<net::InventoryVector> requests;
        const auto request_missing = [this, &requests](const crypto::Hash256& hash) {
            if (!chain_.GetBlockIndex(hash).has_value() &&
                std::none_of(requests.begin(), requests.end(),
                             [&hash](const auto& request) { return request.hash == hash; })) {
                requests.push_back({parameters_.block_inventory_type, hash});
            }
        };
        for (const auto& header : headers.headers) {
            const auto hash = primitives::ComputeBlockHash(header);
            if (!hash.has_value())
                return {SyncError::kWorkFailure, {}, {}};
            if (headers_.contains(*hash)) {
                // Previously validated header: overlap is idempotent, but its
                // missing body must be requested again after disconnect/loss.
                request_missing(*hash);
                continue;
            }
            const auto parent = headers_.find(header.previous_block_id);
            if (parent == headers_.end())
                return {SyncError::kUnknownHeaderParent, {}, {}};
            if (parent->second.height == std::numeric_limits<std::uint32_t>::max()) {
                return {SyncError::kWorkOverflow, {}, {}};
            }
            const auto expected_bits = ExpectedBits(parent->second, header.time);
            if (!expected_bits.has_value() || header.bits != *expected_bits) {
                return {SyncError::kInvalidTarget, {}, {}};
            }
            const auto target =
                consensus::TargetFromCompact(header.bits, parameters_.validation.pow_parameters);
            if (target.error != consensus::TargetError::kNone || !target.target.has_value())
                return {SyncError::kInvalidTarget, {}, {}};
            if (!consensus::CheckProofOfWork(*hash, *target.target))
                return {SyncError::kBadProofOfWork, {}, {}};
            const auto work = consensus::CalculateWork(*target.target);
            if (!work.has_value())
                return {SyncError::kWorkFailure, {}, {}};
            auto chain_work = parent->second.chain_work;
            if (!chain_work.Add(*work))
                return {SyncError::kWorkOverflow, {}, {}};
            const HeaderIndex index{*hash,       parent->second.hash, parent->second.height + 1U,
                                    header.time, header.bits,         *target.target,
                                    *work,       chain_work};
            batch.added.push_back(*hash);
            headers_.emplace(*hash, index);
            const auto best = headers_.find(best_header_tip_);
            if (best == headers_.end() || best->second.chain_work < chain_work ||
                (best->second.chain_work == chain_work && HashLess{}(*hash, best_header_tip_)))
                best_header_tip_ = *hash;
            request_missing(*hash);
        }
        SyncResult result;
        if (!requests.empty()) {
            result.requests.push_back(
                {net::Command::kGetData, net::InventoryMessage{std::move(requests)}});
        }
        batch.committed = true;
        return result;
    } catch (...) {
        return {SyncError::kAllocationFailure, {}, {}};
    }
}

std::optional<std::uint32_t>
Synchronizer::ExpectedBits(const HeaderIndex& parent,
                           const std::uint32_t candidate_time) const noexcept
{
    const auto& difficulty = parameters_.difficulty;
    const auto pow_limit = parameters_.validation.pow_parameters.pow_limit_compact;
    if (parent.height == std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    const auto child_height = parent.height + 1U;
    if (difficulty.no_retargeting)
        return pow_limit;
    if (child_height % difficulty.retarget_interval != 0U) {
        const auto delayed = candidate_time > parent.time &&
                             candidate_time - parent.time > difficulty.target_spacing_seconds * 2U;
        if (difficulty.allow_min_difficulty_blocks && delayed)
            return pow_limit;
        if (!difficulty.allow_min_difficulty_blocks)
            return parent.bits;
        const HeaderIndex* current = &parent;
        while (current->height % difficulty.retarget_interval != 0U && current->bits == pow_limit) {
            const auto previous = headers_.find(current->previous_hash);
            if (previous == headers_.end())
                break;
            const auto is_delayed =
                current->time > previous->second.time &&
                current->time - previous->second.time > difficulty.target_spacing_seconds * 2U;
            if (!is_delayed)
                break;
            current = &previous->second;
        }
        return current->bits;
    }
    if (child_height < difficulty.retarget_interval)
        return std::nullopt;
    const auto first_height = child_height - difficulty.retarget_interval;
    const HeaderIndex* first = &parent;
    while (first->height > first_height) {
        const auto previous = headers_.find(first->previous_hash);
        if (previous == headers_.end())
            return std::nullopt;
        first = &previous->second;
    }
    if (first->height != first_height)
        return std::nullopt;
    const auto old_target =
        consensus::TargetFromCompact(parent.bits, parameters_.validation.pow_parameters);
    if (!old_target.target.has_value())
        return std::nullopt;
    const auto elapsed = parent.time > first->time ? parent.time - first->time : 0U;
    const auto actual = std::clamp(elapsed, difficulty.target_timespan_seconds / 4U,
                                   difficulty.target_timespan_seconds * 4U);
    const auto scaled = ScaleTarget(*old_target.target, actual, difficulty.target_timespan_seconds);
    const auto target =
        !scaled.has_value() || parameters_.validation.pow_parameters.pow_limit < *scaled
            ? parameters_.validation.pow_parameters.pow_limit
            : *scaled;
    return consensus::CompactFromTarget(target);
}

SyncResult Synchronizer::AcceptBlock(const primitives::Block& block) noexcept
{
    const auto result = chain_.AcceptBlock(block);
    return result.error == chain::ChainError::kNone
               ? SyncResult{SyncError::kNone, result, {}}
               : SyncResult{SyncError::kBlockRejected, result, {}};
}

} // namespace nova::node
