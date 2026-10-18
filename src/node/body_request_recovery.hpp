#pragma once

#include "net/transport.hpp"

#include <map>
#include <memory_resource>
#include <span>

namespace nova::node
{

// Volatile request lifecycle. Admission is never a delivery acknowledgement.
class BodyRequestRecovery final
{
  public:
    static constexpr std::size_t kCapacity = 4096U;
    static constexpr std::size_t kBatch = 2000U;
    enum class BodyState : std::uint8_t { kPending, kValidated, kStale };
    explicit BodyRequestRecovery(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept;
    struct Counters final {
        std::uint64_t queue_failures{}, retry_attempts{}, retry_admitted{};
        std::uint64_t expired{}, disconnected{}, exhausted{}, overflow{};
        std::uint64_t allocation_failures{}, clock_resets{}, snapshot_failures{};
        std::uint64_t completed{}, stale{}, body_exhausted{}, lifecycle_expired{};
    };
    class Sink
    {
      public:
        virtual ~Sink() = default;
        [[nodiscard]] virtual BodyState State(const crypto::Hash256& hash) const noexcept = 0;
        [[nodiscard]] virtual net::TransportError
        QueueBody(std::uint64_t peer, const net::FramedMessage& message) noexcept = 0;
    };
    void Submit(std::uint64_t peer, const net::InventoryMessage& inventory, std::uint64_t now,
                Sink& sink) noexcept;
    void Tick(std::uint64_t now, std::span<const net::TransportPeer> peers, Sink& sink) noexcept;
    void SnapshotUnavailable() noexcept;
    [[nodiscard]] std::size_t size() const noexcept
    {
        return entries_.size();
    }
    [[nodiscard]] const Counters& counters() const noexcept
    {
        return counters_;
    }

  private:
    struct Less final {
        bool operator()(const crypto::Hash256& left, const crypto::Hash256& right) const noexcept;
    };
    struct Entry final {
        std::uint64_t peer{}, born{}, last{}, admitted_at{}, order{};
        std::optional<std::uint64_t> failed_since;
        std::uint32_t failure_retries{}, admissions{};
        bool failed{};
    };
    void Clock(std::uint64_t now) noexcept;
    void Prune(std::uint64_t now, Sink& sink) noexcept;
    void Outcome(const net::InventoryMessage& batch, net::TransportError result, std::uint64_t now,
                 bool retry) noexcept;
    std::pmr::map<crypto::Hash256, Entry, Less> entries_;
    std::uint64_t next_order_{};
    std::optional<std::uint64_t> clock_, last_retry_;
    Counters counters_;
};

} // namespace nova::node
