#pragma once

#include "net/transport.hpp"

#include <compare>
#include <map>

namespace nova::node
{

struct DiscoveryEndpoint final {
    std::array<std::uint8_t, 4U> ip{};
    std::uint16_t port{};
    auto operator<=>(const DiscoveryEndpoint&) const = default;
    [[nodiscard]] static std::optional<DiscoveryEndpoint>
    FromWire(const net::NetworkAddress& address) noexcept;
    [[nodiscard]] static std::optional<DiscoveryEndpoint>
    FromNumeric(const net::TcpEndpoint& endpoint) noexcept;
    [[nodiscard]] net::NetworkAddress Wire() const noexcept;
    [[nodiscard]] net::TcpEndpoint Numeric() const;
};

// Node-local policy only. The owner is a single selected REGTEST instance.
// Callers inject elapsed monotonic seconds and entropy; no consensus clock.
class PeerDiscovery final
{
  public:
    explicit PeerDiscovery(std::uint16_t listening_port) noexcept;
    void Update(const std::vector<net::TransportPeer>& peers, std::uint64_t now);
    [[nodiscard]] std::vector<std::uint64_t> Requests(std::uint64_t now);
    void RequestFailed(std::uint64_t id) noexcept;
    [[nodiscard]] std::optional<net::AddressMessage> Reply(std::uint64_t id, std::uint64_t now);
    void Receive(std::uint64_t id, const net::AddressMessage& message, std::uint64_t now);
    [[nodiscard]] std::optional<DiscoveryEndpoint> Next(std::uint64_t now,
                                                        std::optional<std::uint64_t> entropy);
    void Attempt(const DiscoveryEndpoint& endpoint, std::optional<std::uint64_t> id,
                 std::uint64_t now) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    using Ip = std::array<std::uint8_t, 4U>;
    struct Candidate final {
        Ip source;
        std::uint64_t born{};
        std::uint64_t last_attempt{};
        std::uint32_t attempts{};
        std::optional<std::uint64_t> peer;
    };
    struct Session final {
        Ip source;
        std::optional<DiscoveryEndpoint> target;
        bool outbound{};
        bool requested{};
        std::optional<std::uint64_t> response_since;
    };
    struct SourceWindow final {
        std::uint64_t since{};
        bool requested{};
        bool replied{};
    };
    [[nodiscard]] bool Clock(std::uint64_t now) noexcept;
    [[nodiscard]] bool Charge(const Ip& source, bool reply, std::uint64_t now);
    [[nodiscard]] bool Occupied(const DiscoveryEndpoint& endpoint) const noexcept;
    std::uint16_t listening_port_{};
    std::map<DiscoveryEndpoint, Candidate> candidates_;
    std::map<std::uint64_t, Session> sessions_;
    std::map<Ip, SourceWindow> sources_;
    std::vector<net::TransportPeer> peers_;
    std::optional<std::uint64_t> clock_;
    std::optional<std::uint64_t> last_dial_;
    std::uint64_t window_since_{};
    std::uint32_t exchanges_{};
};

} // namespace nova::node
