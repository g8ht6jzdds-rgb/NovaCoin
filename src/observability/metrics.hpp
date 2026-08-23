#pragma once

#include <cstdint>
#include <limits>

namespace nova::observability
{

// Process-local counters are not consensus state, are never persisted, and
// saturate rather than wrapping.
struct MetricsSnapshot final {
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

} // namespace nova::observability
