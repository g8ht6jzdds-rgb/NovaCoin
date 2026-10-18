#include <gtest/gtest.h>

#include "node/body_request_recovery.hpp"

namespace
{
using Recovery = nova::node::BodyRequestRecovery;
using Error = nova::net::TransportError;
using Hash = nova::crypto::Hash256;

Hash MakeHash(const std::uint32_t serial)
{
    Hash::Bytes bytes{};
    for (std::size_t i = 0U; i < 4U; ++i)
        bytes.at(i) = static_cast<std::uint8_t>(serial >> (i * 8U));
    return Hash{bytes};
}

struct Sink final : Recovery::Sink {
    Error result{Error::kQueueLimit};
    std::optional<Hash> validated;
    std::size_t calls{}, count{};
    std::uint64_t peer{};
    Hash first;
    bool Missing(const Hash& hash) const noexcept override
    {
        return !validated.has_value() || *validated != hash;
    }
    Error QueueBody(const std::uint64_t id, const nova::net::FramedMessage& frame) noexcept override
    {
        ++calls;
        peer = id;
        const auto* inventory = std::get_if<nova::net::InventoryMessage>(&frame.message);
        if (frame.command != nova::net::Command::kGetData || inventory == nullptr ||
            inventory->inventory.empty() || inventory->inventory.size() > Recovery::kBatch)
            return Error::kInvalidParameters;
        count = inventory->inventory.size();
        first = inventory->inventory.front().hash;
        return result;
    }
};

std::vector<nova::net::TransportPeer> Peers()
{
    nova::net::TransportPeer peer{};
    peer.id = 7U;
    peer.status.established = true;
    auto second = peer;
    second.id = 8U;
    return {peer, second};
}

nova::net::InventoryMessage Inventory(const std::uint32_t serial)
{
    return {{{2U, MakeHash(serial)}}};
}

TEST(BodyRequestRecovery, RetriesOnlyFailedEnqueueAtDeadlineAndSuccessIsNotDelivery)
{
    Recovery recovery;
    Sink sink;
    recovery.Submit(7U, Inventory(1U), 10U, sink);
    ASSERT_EQ(recovery.size(), 1U);
    recovery.Tick(10U, Peers(), sink);
    EXPECT_EQ(sink.calls, 1U);
    sink.result = Error::kNone;
    recovery.Tick(11U, Peers(), sink);
    EXPECT_EQ(sink.calls, 2U);
    EXPECT_EQ(sink.peer, 7U);
    EXPECT_EQ(sink.first, MakeHash(1U));
    EXPECT_EQ(recovery.size(), 0U);
    recovery.Tick(50U, {}, sink); // accepted then disconnected: deliberately not recovered
    EXPECT_EQ(sink.calls, 2U);
    EXPECT_EQ(recovery.counters().retry_admitted, 1U);
    recovery.Submit(7U, Inventory(2U), 50U, sink); // admitted but withheld: not tracked
    recovery.Tick(100U, Peers(), sink);
    EXPECT_EQ(sink.calls, 3U);
    EXPECT_EQ(recovery.size(), 0U);
}

TEST(BodyRequestRecovery, DuplicateOffersDoNotResetBudgetsAndFailuresStop)
{
    Recovery recovery;
    Sink sink;
    recovery.Submit(7U, Inventory(1U), 0U, sink);
    recovery.Submit(8U, Inventory(1U), 0U, sink);
    EXPECT_EQ(sink.calls, 1U);
    for (std::uint64_t time = 1U; time <= 3U; ++time) {
        recovery.Submit(8U, Inventory(1U), time, sink);
        recovery.Tick(time, Peers(), sink);
        recovery.Tick(time, Peers(), sink);
        EXPECT_EQ(sink.peer, 7U);
    }
    EXPECT_EQ(sink.calls, 4U);
    recovery.Tick(4U, Peers(), sink);
    EXPECT_EQ(recovery.size(), 0U);
    EXPECT_EQ(recovery.counters().exhausted, 1U);
    recovery.Submit(8U, Inventory(1U), 5U, sink); // later explicit reoffer allowed
    EXPECT_EQ(sink.calls, 5U);
}

TEST(BodyRequestRecovery, ExpiryDisconnectRollbackAndSnapshotFailClosed)
{
    Recovery recovery;
    Sink sink;
    recovery.Submit(7U, Inventory(1U), 10U, sink);
    recovery.Submit(7U, Inventory(1U), 39U, sink);
    recovery.Tick(40U, Peers(), sink);
    EXPECT_EQ(sink.calls, 1U);
    EXPECT_EQ(recovery.counters().expired, 1U);
    recovery.Submit(7U, Inventory(1U), 41U, sink);
    recovery.Tick(42U, {}, sink);
    EXPECT_EQ(recovery.counters().disconnected, 1U);
    recovery.Submit(7U, Inventory(1U), 43U, sink);
    recovery.Tick(42U, Peers(), sink);
    EXPECT_EQ(recovery.counters().clock_resets, 1U);
    EXPECT_EQ(recovery.size(), 0U);
    recovery.Submit(7U, Inventory(1U), 44U, sink);
    recovery.SnapshotUnavailable();
    EXPECT_EQ(recovery.size(), 0U);
    EXPECT_EQ(recovery.counters().snapshot_failures, 1U);
}

TEST(BodyRequestRecovery, ValidatedBodySuppressesRetryAndQueueAllocationFailureIsBounded)
{
    Recovery recovery;
    Sink sink;
    sink.result = Error::kAllocationFailure;
    recovery.Submit(7U, Inventory(1U), 0U, sink);
    sink.validated = MakeHash(1U);
    recovery.Tick(1U, Peers(), sink);
    EXPECT_EQ(recovery.size(), 0U);
    EXPECT_EQ(sink.calls, 1U);
    EXPECT_EQ(recovery.counters().queue_failures, 1U);
    recovery.Submit(7U, Inventory(1U), 2U, sink);
    EXPECT_EQ(sink.calls, 1U);
}

TEST(BodyRequestRecovery, StorageFramesAndGlobalPumpBudgetRemainBounded)
{
    Recovery recovery;
    Sink sink;
    nova::net::InventoryMessage inventory;
    for (std::uint32_t start = 0U; start < 6000U; start += 2000U) {
        inventory.inventory.clear();
        for (std::uint32_t i = start; i < start + 2000U; ++i)
            inventory.inventory.push_back({2U, MakeHash(i)});
        recovery.Submit(start == 0U ? 7U : 8U, inventory, 0U, sink);
    }
    EXPECT_EQ(recovery.size(), Recovery::kCapacity);
    EXPECT_EQ(recovery.counters().overflow, 1904U);
    const auto calls = sink.calls;
    sink.result = Error::kNone;
    recovery.Tick(1U, Peers(), sink);
    EXPECT_EQ(sink.calls, calls + 1U);
    EXPECT_LE(sink.count, Recovery::kBatch);
    recovery.Tick(1U, Peers(), sink);
    EXPECT_EQ(sink.calls, calls + 1U);
    inventory.inventory.push_back({2U, MakeHash(9000U)});
    recovery.Submit(7U, inventory, 1U, sink);
    EXPECT_EQ(sink.calls, calls + 1U);
}
} // namespace
