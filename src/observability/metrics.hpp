#pragma once

#include <cstdint>
#include <limits>

namespace nova::observability
{

// Process-local counters are not consensus state, are never persisted, and
// saturate rather than wrapping.
struct MetricsSnapshot final {
    // These values are local operational observations. They are deliberately
    // not used by validation, chain selection, wallet accounting, or storage.
    std::uint64_t uptime_seconds{};
    std::uint64_t last_block_arrival_time_seconds{};
    std::uint64_t blocks_accepted{};
    std::uint64_t blocks_rejected{};
    std::uint64_t transactions_accepted{};
    std::uint64_t transactions_rejected{};
    std::uint64_t p2p_messages_dispatched{};
    std::uint64_t p2p_disconnects{};
    std::uint64_t p2p_transport_errors{};
};

inline void SaturatingAdd(std::uint64_t& destination, const std::uint64_t value) noexcept
{
    if (std::numeric_limits<std::uint64_t>::max() - destination < value) {
        destination = std::numeric_limits<std::uint64_t>::max();
        return;
    }
    destination += value;
}

[[nodiscard]] inline std::uint64_t SaturatingSum(const std::uint64_t left,
                                                 const std::uint64_t right) noexcept
{
    return std::numeric_limits<std::uint64_t>::max() - left < right
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

} // namespace nova::observability
