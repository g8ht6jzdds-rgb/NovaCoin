#include <gtest/gtest.h>

#include "node/body_request_recovery.hpp"

#include <array>
#include <iostream>

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
    std::size_t remaining{100U}, live{}, successful{}, injected_failures{};

  private:
    void* do_allocate(const std::size_t size, const std::size_t alignment) override
    {
        if (remaining == 0U) {
            ++injected_failures;
            throw std::bad_alloc{};
        }
        --remaining;
        auto* result = std::pmr::new_delete_resource()->allocate(size, alignment);
        ++live;
        ++successful;
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

// This fixture never needs more than three hashes. Recording must not allocate
// inside the noexcept callback or affect the separately injected PMR failures.
struct RollbackSink final : Recovery::Sink {
    bool validated{}, invalid_frame{};
    std::size_t calls{}, count{};
    std::uint64_t peer{};
    std::array<Hash, 3> hashes{};
    std::array<std::uint32_t, 3> types{};

    Recovery::BodyState State(const Hash&) const noexcept override
    {
        return validated ? Recovery::BodyState::kValidated : Recovery::BodyState::kPending;
    }
    Error QueueBody(const std::uint64_t id, const nova::net::FramedMessage& frame) noexcept override
    {
        ++calls;
        const auto* inventory = std::get_if<nova::net::InventoryMessage>(&frame.message);
        if (frame.command != nova::net::Command::kGetData || inventory == nullptr ||
            inventory->inventory.empty() || inventory->inventory.size() > hashes.size()) {
            invalid_frame = true;
            return Error::kInvalidParameters;
        }
        peer = id;
        count = inventory->inventory.size();
        for (std::size_t index = 0U; index < count; ++index) {
            hashes[index] = inventory->inventory[index].hash;
            types[index] = inventory->inventory[index].type;
        }
        return Error::kNone;
    }
};

TEST(BodyRequestRecovery, ActualMapAllocationFailureRollsBackBeforeEnqueue)
{
    for (const auto budget : std::array<std::size_t, 3>{0U, 1U, 2U}) {
        SCOPED_TRACE(budget);
        FaultResource resource;
        {
            Recovery recovery{&resource};
            const auto empty_live = resource.live;
            RollbackSink sink;
            recovery.Submit(7U, Inventory(1U), 0U, sink);
            ASSERT_EQ(sink.calls, 1U);
            ASSERT_EQ(sink.peer, 7U);
            ASSERT_EQ(sink.count, 1U);
            ASSERT_EQ(sink.types[0], 2U);
            ASSERT_EQ(sink.hashes[0], MakeHash(1U));
            ASSERT_FALSE(sink.invalid_frame);
            ASSERT_EQ(recovery.size(), 1U);
            const auto seeded_live = resource.live;
            const auto seed_hashes = sink.hashes;
            const auto seed_types = sink.types;
            const auto successful_before = resource.successful;
            const auto injected_before = resource.injected_failures;

            // Arm only after constructing/seeding the map: infrastructure
            // allocations differ by STL and are not application records.
            resource.remaining = budget;
            const nova::net::InventoryMessage batch{
                {{2U, MakeHash(2U)}, {2U, MakeHash(3U)}, {2U, MakeHash(4U)}}};
            recovery.Submit(7U, batch, 0U, sink);
            ASSERT_EQ(resource.injected_failures, injected_before + 1U);
            EXPECT_EQ(resource.successful - successful_before, budget);
            EXPECT_EQ(sink.calls, 1U); // no successful but untracked prefix
            EXPECT_EQ(sink.peer, 7U);
            EXPECT_EQ(sink.count, 1U);
            EXPECT_EQ(sink.hashes, seed_hashes);
            EXPECT_EQ(sink.types, seed_types);
            EXPECT_EQ(recovery.size(), 1U);
            EXPECT_EQ(resource.live, seeded_live);
            EXPECT_EQ(recovery.counters().allocation_failures, 1U);
            EXPECT_EQ(recovery.counters().queue_failures, 0U);
            EXPECT_EQ(recovery.counters().overflow, 0U);
            EXPECT_EQ(recovery.counters().completed, 0U);
            EXPECT_EQ(recovery.counters().retry_attempts, 0U);
            std::cout << "PMR budget=" << budget << " empty_live=" << empty_live
                      << " seeded_live=" << seeded_live << " rollback_live=" << resource.live
                      << " successful_before_failure=" << resource.successful - successful_before
                      << " injected_failures=" << resource.injected_failures - injected_before
                      << '\n';

            resource.remaining = 100U;
            recovery.Submit(7U, Inventory(1U), 1U, sink); // original identity survived
            EXPECT_EQ(sink.calls, 1U);
            EXPECT_EQ(recovery.size(), 1U);
            EXPECT_EQ(resource.live, seeded_live);
            EXPECT_EQ(resource.successful - successful_before, budget);

            recovery.Submit(7U, batch, 1U, sink);
            EXPECT_EQ(sink.calls, 2U);
            EXPECT_EQ(sink.peer, 7U);
            EXPECT_EQ(sink.count, 3U);
            const std::array<Hash, 3> expected_hashes{MakeHash(2U), MakeHash(3U), MakeHash(4U)};
            const std::array<std::uint32_t, 3> expected_types{2U, 2U, 2U};
            EXPECT_EQ(sink.hashes, expected_hashes);
            EXPECT_EQ(sink.types, expected_types);
            EXPECT_FALSE(sink.invalid_frame);
            EXPECT_EQ(recovery.size(), 4U); // failed reservations reclaimed
            EXPECT_EQ(resource.injected_failures, injected_before + 1U);
            EXPECT_EQ(recovery.counters().allocation_failures, 1U);
            EXPECT_EQ(recovery.counters().queue_failures, 0U);
            EXPECT_EQ(recovery.counters().overflow, 0U);
            EXPECT_EQ(recovery.counters().completed, 0U);

            constexpr char kHexDigits[]{"0123456789abcdef"};
            std::cout << "PMR budget=" << budget << " reoffer_peer=" << sink.peer
                      << " reoffer_count=" << sink.count << " reoffer_size=" << recovery.size()
                      << " type:hash=";
            for (std::size_t index = 0U; index < sink.hashes.size(); ++index) {
                std::cout << ' ' << sink.types[index] << ':';
                for (const auto byte : sink.hashes[index].bytes())
                    std::cout << kHexDigits[byte >> 4U] << kHexDigits[byte & 0x0fU];
            }
            std::cout << '\n';

            sink.validated = true;
            recovery.Tick(2U, Peers(), sink); // before any retry deadline
            EXPECT_EQ(sink.calls, 2U);
            EXPECT_EQ(recovery.size(), 0U);
            EXPECT_EQ(recovery.counters().completed, 4U);
            EXPECT_EQ(recovery.counters().retry_attempts, 0U);
            EXPECT_EQ(resource.live, empty_live); // map infrastructure still alive
            std::cout << "PMR budget=" << budget << " cleanup_size=" << recovery.size()
                      << " cleanup_live=" << resource.live << '\n';
        }
        EXPECT_EQ(resource.live, 0U);
        std::cout << "PMR budget=" << budget << " after_destroy_live=" << resource.live << '\n';
    }
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
