#include "node/body_request_recovery.hpp"

#include <algorithm>
#include <limits>

namespace nova::node
{
namespace
{
void Increment(std::uint64_t& value) noexcept
{
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}
} // namespace

BodyRequestRecovery::BodyRequestRecovery(std::pmr::memory_resource* resource) noexcept
    : entries_(resource == nullptr ? std::pmr::get_default_resource() : resource)
{
}

bool BodyRequestRecovery::Less::operator()(const crypto::Hash256& left,
                                           const crypto::Hash256& right) const noexcept
{
    return std::lexicographical_compare(left.bytes().begin(), left.bytes().end(),
                                        right.bytes().begin(), right.bytes().end());
}

void BodyRequestRecovery::Clock(const std::uint64_t now) noexcept
{
    if (clock_.has_value() && now < *clock_) {
        entries_.clear();
        last_retry_.reset();
        next_order_ = 0U;
        Increment(counters_.clock_resets);
    }
    clock_ = now;
}

void BodyRequestRecovery::Prune(const std::uint64_t now, Sink& sink) noexcept
{
    for (auto item = entries_.begin(); item != entries_.end();) {
        const auto& entry = item->second;
        const auto state = sink.State(item->first);
        std::uint64_t* reason = nullptr;
        if (state == BodyState::kValidated)
            reason = &counters_.completed;
        else if (state == BodyState::kStale)
            reason = &counters_.stale;
        else if (now - entry.born >= 60U)
            reason = &counters_.lifecycle_expired;
        else if (entry.failed_since && now - *entry.failed_since >= 30U)
            reason = &counters_.expired;
        else if (entry.failed && entry.failure_retries >= 3U)
            reason = &counters_.exhausted;
        else if (!entry.failed && entry.admissions >= 3U && now - entry.admitted_at >= 10U)
            reason = &counters_.body_exhausted;
        if (reason != nullptr) {
            Increment(*reason);
            item = entries_.erase(item);
        } else {
            ++item;
        }
    }
    if (entries_.empty())
        next_order_ = 0U;
}

void BodyRequestRecovery::Outcome(const net::InventoryMessage& batch,
                                  const net::TransportError result, const std::uint64_t now,
                                  const bool retry) noexcept
{
    if (result != net::TransportError::kNone)
        Increment(counters_.queue_failures);
    else if (retry)
        Increment(counters_.retry_admitted); // admission, never completion
    for (const auto& item : batch.inventory) {
        auto& entry = entries_.find(item.hash)->second;
        if (retry && entry.failed)
            ++entry.failure_retries;
        entry.last = now;
        entry.failed = result != net::TransportError::kNone;
        if (entry.failed) {
            if (!entry.failed_since)
                entry.failed_since = now;
        } else {
            ++entry.admissions;
            entry.admitted_at = now;
        }
    }
}

void BodyRequestRecovery::Submit(const std::uint64_t peer, const net::InventoryMessage& inventory,
                                 const std::uint64_t now, Sink& sink) noexcept
{
    Clock(now);
    Prune(now, sink);
    if (inventory.inventory.size() > kBatch) {
        Increment(counters_.overflow);
        return;
    }
    try {
        net::InventoryMessage batch;
        batch.inventory.reserve(inventory.inventory.size());
        for (const auto& item : inventory.inventory) {
            if (item.type == 2U && sink.State(item.hash) == BodyState::kPending &&
                !entries_.contains(item.hash) &&
                std::none_of(batch.inventory.begin(), batch.inventory.end(),
                             [&item](const auto& other) { return other.hash == item.hash; }))
                batch.inventory.push_back(item);
        }
        if (batch.inventory.empty())
            return;
        if (batch.inventory.size() > kCapacity - entries_.size() ||
            batch.inventory.size() > std::numeric_limits<std::uint64_t>::max() - next_order_) {
            for (std::size_t i = 0U; i < batch.inventory.size(); ++i)
                Increment(counters_.overflow);
            return;
        }
        const net::FramedMessage frame{net::Command::kGetData, batch};
        struct Reservation final {
            decltype(entries_)& entries;
            std::uint64_t& order;
            std::uint64_t previous;
            const net::InventoryMessage& batch;
            bool committed{};
            ~Reservation()
            {
                if (!committed) {
                    for (const auto& item : batch.inventory)
                        entries.erase(item.hash);
                    order = previous;
                }
            }
        } reservation{entries_, next_order_, next_order_, batch, false};
        for (const auto& item : batch.inventory) {
            Entry entry;
            entry.peer = peer;
            entry.born = entry.last = now;
            entry.order = next_order_++;
            entries_.emplace(item.hash, entry);
        }
        const auto result = sink.QueueBody(peer, frame);
        Outcome(batch, result, now, false);
        reservation.committed = true;
    } catch (...) {
        Increment(counters_.allocation_failures);
    }
}

void BodyRequestRecovery::Tick(const std::uint64_t now,
                               const std::span<const net::TransportPeer> peers, Sink& sink) noexcept
{
    Clock(now);
    Prune(now, sink);
    for (auto item = entries_.begin(); item != entries_.end();) {
        const auto alive = std::any_of(peers.begin(), peers.end(), [&item](const auto& peer) {
            return peer.id == item->second.peer && peer.status.established;
        });
        if (!alive) {
            Increment(counters_.disconnected);
            item = entries_.erase(item);
        } else {
            ++item;
        }
    }
    if (last_retry_ && now <= *last_retry_)
        return;
    try {
        const auto due = [now](const Entry& entry) {
            return entry.failed ? now > entry.last : now - entry.admitted_at >= 10U;
        };
        auto first = entries_.end();
        for (auto item = entries_.begin(); item != entries_.end(); ++item) {
            if (due(item->second) &&
                (first == entries_.end() || item->second.order < first->second.order))
                first = item;
        }
        if (first == entries_.end())
            return;
        const auto peer = first->second.peer;
        net::InventoryMessage batch;
        batch.inventory.reserve(entries_.size());
        for (const auto& [hash, entry] : entries_) {
            if (entry.peer == peer && due(entry))
                batch.inventory.push_back({2U, hash});
        }
        std::sort(
            batch.inventory.begin(), batch.inventory.end(), [this](const auto& a, const auto& b) {
                return entries_.find(a.hash)->second.order < entries_.find(b.hash)->second.order;
            });
        if (batch.inventory.size() > kBatch)
            batch.inventory.resize(kBatch);
        const net::FramedMessage frame{net::Command::kGetData, batch};
        last_retry_ = now;
        Increment(counters_.retry_attempts);
        Outcome(batch, sink.QueueBody(peer, frame), now, true);
    } catch (...) {
        last_retry_ = now;
        Increment(counters_.allocation_failures);
    }
}

void BodyRequestRecovery::SnapshotUnavailable() noexcept
{
    entries_.clear();
    Increment(counters_.snapshot_failures);
}
} // namespace nova::node
