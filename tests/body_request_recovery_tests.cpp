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
    bool stale{};
    std::size_t calls{}, count{};
    std::uint64_t peer{};
    Hash first;
    Recovery::BodyState State(const Hash& hash) const noexcept override
    {
        if (validated.has_value() && *validated == hash)
            return Recovery::BodyState::kValidated;
        return stale ? Recovery::BodyState::kStale : Recovery::BodyState::kPending;
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

TEST(BodyRequestRecovery, FailedEnqueueAdmissionIsNotCompletionAndDisconnectCancels)
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
    EXPECT_EQ(recovery.size(), 1U); // admitted, body still absent
    EXPECT_EQ(recovery.counters().completed, 0U);
    recovery.Tick(12U, {}, sink); // no blind alternate request after disconnect
    EXPECT_EQ(sink.calls, 2U);
    EXPECT_EQ(recovery.counters().retry_admitted, 1U);
    EXPECT_EQ(recovery.size(), 0U);
    EXPECT_EQ(recovery.counters().disconnected, 1U);
    recovery.Submit(8U, Inventory(1U), 13U, sink); // explicit reoffer, not reassignment
    recovery.Tick(13U, Peers(), sink);
    EXPECT_EQ(sink.calls, 3U);
    EXPECT_EQ(recovery.size(), 1U);
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
    EXPECT_EQ(recovery.size(), 4000U); // entire untrackable batch rejected before Queue
    EXPECT_EQ(recovery.counters().overflow, 2000U);
    inventory.inventory.resize(96U);
    recovery.Submit(8U, inventory, 0U, sink);
    EXPECT_EQ(recovery.size(), Recovery::kCapacity);
    const auto calls = sink.calls;
    sink.result = Error::kNone;
    recovery.Tick(1U, Peers(), sink);
    EXPECT_EQ(sink.calls, calls + 1U);
    EXPECT_LE(sink.count, Recovery::kBatch);
    recovery.Tick(1U, Peers(), sink);
    EXPECT_EQ(sink.calls, calls + 1U);
    inventory.inventory.resize(2001U);
    recovery.Submit(7U, inventory, 1U, sink);
    EXPECT_EQ(sink.calls, calls + 1U);
}

TEST(BodyRequestRecovery, AdmittedDeadlinesAreBoundedAndOnlyValidationCompletes)
{
    Recovery recovery;
    Sink sink;
    sink.result = Error::kNone;
    recovery.Submit(7U, Inventory(1U), 0U, sink);
    recovery.Submit(8U, Inventory(1U), 9U, sink);
    recovery.Tick(9U, Peers(), sink);
    EXPECT_EQ(sink.calls, 1U);
    for (const std::uint64_t now : {10U, 20U}) {
        recovery.Tick(now, Peers(), sink);
        recovery.Tick(now, Peers(), sink);
        EXPECT_EQ(sink.peer, 7U);
        EXPECT_EQ(recovery.counters().completed, 0U);
    }
    EXPECT_EQ(sink.calls, 3U);
    EXPECT_EQ(recovery.size(), 1U);
    recovery.Tick(29U, Peers(), sink);
    EXPECT_EQ(recovery.size(), 1U); // final admitted request has a response window
    recovery.Tick(30U, Peers(), sink);
    EXPECT_EQ(recovery.size(), 0U);
    EXPECT_EQ(recovery.counters().body_exhausted, 1U);
    EXPECT_EQ(recovery.counters().completed, 0U);
    recovery.Submit(7U, Inventory(2U), 31U, sink);
    sink.validated = MakeHash(2U);
    recovery.Tick(41U, Peers(), sink);
    recovery.Tick(42U, Peers(), sink);
    EXPECT_EQ(recovery.counters().completed, 1U);
    EXPECT_EQ(sink.calls, 4U);
}

TEST(BodyRequestRecovery, PhaseChangesDoNotResetFailureAgeOrAttempts)
{
    Recovery recovery;
    Sink sink;
    recovery.Submit(7U, Inventory(1U), 0U, sink);
    sink.result = Error::kNone;
    recovery.Tick(1U, Peers(), sink); // failed-phase retry #1, first admission
    sink.result = Error::kQueueLimit;
    recovery.Tick(11U, Peers(), sink); // deadline resend fails, no new failure epoch
    recovery.Tick(12U, Peers(), sink); // failed-phase retry #2
    sink.result = Error::kNone;
    recovery.Tick(13U, Peers(), sink); // failed-phase retry #3, second admission
    EXPECT_EQ(recovery.size(), 1U);
    recovery.Tick(23U, Peers(), sink); // third admission
    recovery.Tick(30U, Peers(), sink); // original failure age still applies
    EXPECT_EQ(recovery.size(), 0U);
    EXPECT_EQ(recovery.counters().expired, 1U);
    EXPECT_EQ(recovery.counters().completed, 0U);
    EXPECT_EQ(sink.calls, 6U);
}

TEST(BodyRequestRecovery, StaleWorkAndHardAgeAreNotCompletion)
{
    Recovery recovery;
    Sink sink;
    sink.result = Error::kNone;
    recovery.Submit(7U, Inventory(1U), 0U, sink);
    sink.stale = true;
    recovery.Tick(1U, Peers(), sink);
    EXPECT_EQ(recovery.counters().stale, 1U);
    sink.stale = false;
    recovery.Submit(7U, Inventory(2U), 2U, sink);
    recovery.Tick(62U, Peers(), sink);
    EXPECT_EQ(recovery.counters().lifecycle_expired, 1U);
    EXPECT_EQ(recovery.counters().completed, 0U);
    EXPECT_EQ(sink.calls, 2U);
}

class FaultResource final : public std::pmr::memory_resource
{
  public:
    std::size_t remaining{100U}, live{};

  private:
    void* do_allocate(const std::size_t size, const std::size_t alignment) override
    {
        if (remaining == 0U)
            throw std::bad_alloc{};
        --remaining;
        auto* result = std::pmr::new_delete_resource()->allocate(size, alignment);
        ++live;
        return result;
    }
    void do_deallocate(void* pointer, const std::size_t size, const std::size_t alignment) override
    {
        --live;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
    {
        return this == &other;
    }
};

TEST(BodyRequestRecovery, ActualMapAllocationFailureRollsBackBeforeEnqueue)
{
    FaultResource resource;
    {
        Recovery recovery{&resource};
        Sink sink;
        sink.result = Error::kNone;
        recovery.Submit(7U, Inventory(1U), 0U, sink);
        resource.remaining = 2U;
        const nova::net::InventoryMessage batch{
            {{2U, MakeHash(2U)}, {2U, MakeHash(3U)}, {2U, MakeHash(4U)}}};
        recovery.Submit(7U, batch, 0U, sink);
        EXPECT_EQ(sink.calls, 1U); // no successful but untracked prefix
        EXPECT_EQ(recovery.size(), 1U);
        EXPECT_EQ(resource.live, 1U);
        EXPECT_EQ(recovery.counters().allocation_failures, 1U);
        resource.remaining = 100U;
        recovery.Submit(7U, batch, 1U, sink);
        EXPECT_EQ(sink.calls, 2U);
        EXPECT_EQ(recovery.size(), 4U);
    }
    EXPECT_EQ(resource.live, 0U);
}

TEST(BodyRequestRecovery, RetryRetainsOriginalBatchOrderAndSharesOnePhaseBudget)
{
    Recovery recovery;
    Sink sink;
    sink.result = Error::kNone;
    const nova::net::InventoryMessage batch{{{2U, MakeHash(1U)}, {2U, MakeHash(256U)}}};
    recovery.Submit(7U, batch, 0U, sink);
    sink.result = Error::kQueueLimit;
    recovery.Submit(8U, Inventory(3U), 9U, sink);
    sink.result = Error::kNone;
    const auto calls = sink.calls;
    recovery.Tick(10U, Peers(), sink); // older admitted batch wins this one frame
    EXPECT_EQ(sink.calls, calls + 1U);
    EXPECT_EQ(sink.peer, 7U);
    EXPECT_EQ(sink.first, MakeHash(1U)); // NOT map/hash order (256 would sort first)
    recovery.Tick(10U, Peers(), sink);
    EXPECT_EQ(sink.calls, calls + 1U);
    recovery.Tick(11U, Peers(), sink);
    EXPECT_EQ(sink.peer, 8U);
    EXPECT_EQ(sink.calls, calls + 2U);
}
} // namespace
