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
        Increment(counters_.clock_resets);
    }
    clock_ = now;
}

void BodyRequestRecovery::Submit(const std::uint64_t peer, const net::InventoryMessage& inventory,
                                 const std::uint64_t now, Sink& sink) noexcept
{
    Clock(now);
    if (inventory.inventory.size() > kBatch) {
        Increment(counters_.overflow);
        return;
    }
    try {
        net::InventoryMessage batch;
        batch.inventory.reserve(inventory.inventory.size());
        for (const auto& item : inventory.inventory) {
            if (item.type == 2U && sink.Missing(item.hash) && !entries_.contains(item.hash) &&
                std::none_of(batch.inventory.begin(), batch.inventory.end(),
                             [&item](const auto& other) { return other.hash == item.hash; }))
                batch.inventory.push_back(item);
        }
        if (batch.inventory.empty())
            return;
        if (sink.QueueBody(peer, {net::Command::kGetData, batch}) == net::TransportError::kNone)
            return;
        Increment(counters_.queue_failures);
        for (const auto& item : batch.inventory) {
            if (entries_.size() >= kCapacity) {
                Increment(counters_.overflow);
                continue;
            }
            entries_.emplace(item.hash, Entry{peer, now, now, 0U});
        }
    } catch (...) {
        // Any installed prefix remains bounded and recoverable. Headers unchanged.
        Increment(counters_.allocation_failures);
    }
}

void BodyRequestRecovery::Tick(const std::uint64_t now,
                               const std::span<const net::TransportPeer> peers, Sink& sink) noexcept
{
    Clock(now);
    for (auto item = entries_.begin(); item != entries_.end();) {
        const auto& entry = item->second;
        const bool alive = std::any_of(peers.begin(), peers.end(), [&entry](const auto& peer) {
            return peer.id == entry.peer && peer.status.established;
        });
        bool remove = !sink.Missing(item->first);
        if (!remove && !alive) {
            Increment(counters_.disconnected);
            remove = true;
        } else if (!remove && now - entry.born >= 30U) {
            Increment(counters_.expired);
            remove = true;
        } else if (!remove && entry.attempts >= 3U) {
            Increment(counters_.exhausted);
            remove = true;
        }
        if (remove)
            item = entries_.erase(item);
        else
            ++item;
    }
    if (last_retry_.has_value() && now <= *last_retry_)
        return;
    try {
        const auto first = std::find_if(entries_.begin(), entries_.end(),
                                        [now](const auto& item) { return now > item.second.last; });
        if (first == entries_.end())
            return;
        const auto peer = first->second.peer;
        net::InventoryMessage batch;
        batch.inventory.reserve(std::min(kBatch, entries_.size()));
        for (const auto& [hash, entry] : entries_) {
            if (entry.peer == peer && now > entry.last && batch.inventory.size() < kBatch)
                batch.inventory.push_back({2U, hash});
        }
        // Build the frame before charging attempts; construction may allocate.
        const net::FramedMessage frame{net::Command::kGetData, batch};
        last_retry_ = now;
        Increment(counters_.retry_attempts);
        const auto result = sink.QueueBody(peer, frame);
        if (result != net::TransportError::kNone)
            Increment(counters_.queue_failures);
        else
            Increment(counters_.retry_admitted);
        for (const auto& item : batch.inventory) {
            const auto found = entries_.find(item.hash);
            if (result == net::TransportError::kNone) {
                entries_.erase(found);
            } else {
                found->second.last = now;
                ++found->second.attempts;
            }
        }
    } catch (...) {
        // Bound allocation-failure retries too, including at the same timestamp.
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
