#include "node/peer_discovery.hpp"

#include <algorithm>
#include <limits>

namespace nova::node
{
namespace
{
constexpr std::array<std::uint8_t, 4U> kLoopback{127U, 0U, 0U, 1U};
constexpr std::size_t kCapacity = 256U;
constexpr std::size_t kBatch = 16U;
constexpr std::uint64_t kLifetime = 1'800U;
} // namespace

std::optional<DiscoveryEndpoint>
DiscoveryEndpoint::FromWire(const net::NetworkAddress& address) noexcept
{
    const auto& bytes = address.address;
    if (address.port == 0U ||
        !std::all_of(bytes.begin(), bytes.begin() + 10,
                     [](const auto byte) { return byte == 0U; }) ||
        bytes[10] != 255U || bytes[11] != 255U ||
        !std::equal(kLoopback.begin(), kLoopback.end(), bytes.begin() + 12)) {
        return std::nullopt;
    }
    return DiscoveryEndpoint{kLoopback, address.port};
}

std::optional<DiscoveryEndpoint>
DiscoveryEndpoint::FromNumeric(const net::TcpEndpoint& endpoint) noexcept
{
    return endpoint.host == "127.0.0.1" && endpoint.port != 0U
               ? std::optional<DiscoveryEndpoint>{{kLoopback, endpoint.port}}
               : std::nullopt;
}

net::NetworkAddress DiscoveryEndpoint::Wire() const noexcept
{
    net::NetworkAddress result{};
    result.address[10] = 255U;
    result.address[11] = 255U;
    std::copy(ip.begin(), ip.end(), result.address.begin() + 12);
    result.port = port;
    return result;
}

net::TcpEndpoint DiscoveryEndpoint::Numeric() const
{
    return {std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) +
                "." + std::to_string(ip[3]),
            port};
}

PeerDiscovery::PeerDiscovery(const std::uint16_t listening_port) noexcept
    : listening_port_(listening_port)
{
}

bool PeerDiscovery::Clock(const std::uint64_t now) noexcept
{
    if (clock_.has_value() && now < *clock_) {
        // Rollback fails closed; do not clear quotas and grant a fresh budget.
        return false;
    }
    clock_ = now;
    return true;
}

void PeerDiscovery::Update(const std::vector<net::TransportPeer>& peers, const std::uint64_t now)
{
    if (!Clock(now) || peers.size() > 80U) {
        return;
    }
    peers_ = peers;
    std::erase_if(sessions_, [&peers](const auto& item) {
        return std::none_of(peers.begin(), peers.end(), [&item](const auto& peer) {
            return peer.id == item.first && peer.status.established;
        });
    });
    for (const auto& peer : peers) {
        if (peer.status.established && peer.source_ipv4.has_value() &&
            *peer.source_ipv4 == kLoopback && !sessions_.contains(peer.id)) {
            const auto target = peer.outbound_target.has_value()
                                    ? DiscoveryEndpoint::FromNumeric(*peer.outbound_target)
                                    : std::nullopt;
            sessions_.emplace(peer.id,
                              Session{*peer.source_ipv4, target,
                                      peer.status.direction == net::ConnectionDirection::kOutbound,
                                      false, std::nullopt});
        }
    }
    for (auto& [endpoint, candidate] : candidates_) {
        static_cast<void>(endpoint);
        if (candidate.peer.has_value() &&
            std::none_of(peers.begin(), peers.end(),
                         [&candidate](const auto& peer) { return peer.id == *candidate.peer; })) {
            candidate.peer.reset();
            candidate.last_attempt = now;
        }
    }
    std::erase_if(candidates_, [now](const auto& item) {
        return !item.second.peer.has_value() && now - item.second.born >= kLifetime;
    });
    std::erase_if(sources_, [now](const auto& item) { return now - item.second.since >= 60U; });
}

bool PeerDiscovery::Charge(const Ip& source, const bool reply, const std::uint64_t now)
{
    if (!Clock(now)) {
        return false;
    }
    if (now - window_since_ >= 60U) {
        window_since_ = now;
        exchanges_ = 0U;
    }
    if (exchanges_ >= 16U) {
        return false;
    }
    auto found = sources_.find(source);
    if (found == sources_.end()) {
        if (sources_.size() >= kCapacity) {
            return false;
        }
        found = sources_.emplace(source, SourceWindow{now, false, false}).first;
    }
    if (now - found->second.since >= 60U) {
        found->second = {now, false, false};
    }
    auto& spent = reply ? found->second.replied : found->second.requested;
    if (spent) {
        return false;
    }
    spent = true;
    ++exchanges_;
    return true;
}

std::vector<std::uint64_t> PeerDiscovery::Requests(const std::uint64_t now)
{
    std::vector<std::uint64_t> result;
    if (!Clock(now)) {
        return result;
    }
    for (auto& [id, session] : sessions_) {
        if (session.outbound && !session.requested && Charge(session.source, false, now)) {
            session.requested = true;
            session.response_since = now;
            result.push_back(id);
        }
    }
    return result;
}

void PeerDiscovery::RequestFailed(const std::uint64_t id) noexcept
{
    const auto found = sessions_.find(id);
    if (found != sessions_.end()) {
        found->second.response_since.reset();
    }
}

std::optional<net::AddressMessage> PeerDiscovery::Reply(const std::uint64_t id,
                                                        const std::uint64_t now)
{
    const auto requester = sessions_.find(id);
    if (requester == sessions_.end() || !Charge(requester->second.source, true, now)) {
        return std::nullopt;
    }
    net::AddressMessage result;
    for (const auto& [other_id, session] : sessions_) {
        if (other_id != id && session.outbound && session.target.has_value() &&
            session.target->port != listening_port_ && result.addresses.size() < kBatch &&
            (!requester->second.target.has_value() ||
             *session.target != *requester->second.target)) {
            result.addresses.push_back(session.target->Wire());
        }
    }
    return result;
}

bool PeerDiscovery::Occupied(const DiscoveryEndpoint& endpoint) const noexcept
{
    return endpoint.port == listening_port_ ||
           std::any_of(peers_.begin(), peers_.end(), [&endpoint](const auto& peer) {
               return peer.outbound_target.has_value() &&
                      (DiscoveryEndpoint::FromNumeric(*peer.outbound_target) == endpoint ||
                       (peer.status.direction == net::ConnectionDirection::kOutbound &&
                        peer.source_ipv4 == endpoint.ip &&
                        peer.outbound_target->port == endpoint.port));
           });
}

void PeerDiscovery::Receive(const std::uint64_t id, const net::AddressMessage& message,
                            const std::uint64_t now)
{
    const auto found = sessions_.find(id);
    if (!Clock(now) || found == sessions_.end() || !found->second.response_since.has_value()) {
        return;
    }
    const auto started = *found->second.response_since;
    found->second.response_since.reset();
    if (now - started >= 10U) {
        return;
    }
    for (std::size_t index = 0U; index < std::min(kBatch, message.addresses.size()); ++index) {
        const auto endpoint = DiscoveryEndpoint::FromWire(message.addresses[index]);
        if (!endpoint.has_value() || Occupied(*endpoint) || candidates_.contains(*endpoint) ||
            candidates_.size() >= kCapacity) {
            continue;
        }
        const auto from_source =
            std::count_if(candidates_.begin(), candidates_.end(), [&found](const auto& item) {
                return item.second.source == found->second.source;
            });
        const auto in_group =
            std::count_if(candidates_.begin(), candidates_.end(), [&endpoint](const auto& item) {
                return item.first.ip[0] == endpoint->ip[0] && item.first.ip[1] == endpoint->ip[1];
            });
        if (from_source < 16 && in_group < 64) {
            candidates_.emplace(*endpoint,
                                Candidate{found->second.source, now, 0U, 0U, std::nullopt});
        }
    }
}

std::optional<DiscoveryEndpoint> PeerDiscovery::Next(const std::uint64_t now,
                                                     const std::optional<std::uint64_t> entropy)
{
    if (!Clock(now) || !entropy.has_value() || (last_dial_.has_value() && now - *last_dial_ < 1U)) {
        return std::nullopt;
    }
    const auto outbound = std::count_if(peers_.begin(), peers_.end(), [](const auto& peer) {
        return peer.status.direction == net::ConnectionDirection::kOutbound;
    });
    const auto discovered =
        std::count_if(candidates_.begin(), candidates_.end(),
                      [](const auto& item) { return item.second.peer.has_value(); });
    if (outbound >= 14 || discovered >= 2) { // reserve two slots for explicit peers
        return std::nullopt;
    }
    if (std::any_of(peers_.begin(), peers_.end(), [](const auto& peer) {
            return peer.status.direction == net::ConnectionDirection::kOutbound &&
                   !peer.status.established && !peer.source_ipv4.has_value() &&
                   peer.outbound_target.has_value() &&
                   !DiscoveryEndpoint::FromNumeric(*peer.outbound_target).has_value();
        })) {
        return std::nullopt; // wait for explicit hostname targets to become identifiable
    }
    std::vector<DiscoveryEndpoint> eligible;
    for (const auto& [endpoint, candidate] : candidates_) {
        const auto group_peers =
            std::count_if(peers_.begin(), peers_.end(), [&endpoint](const auto& peer) {
                if (peer.status.direction != net::ConnectionDirection::kOutbound ||
                    !peer.outbound_target.has_value()) {
                    return false;
                }
                const auto target = DiscoveryEndpoint::FromNumeric(*peer.outbound_target);
                const auto ip = peer.source_ipv4.has_value() ? peer.source_ipv4
                                : target.has_value()         ? std::optional<Ip>{target->ip}
                                                             : std::nullopt;
                return ip.has_value() && (*ip)[0] == endpoint.ip[0] && (*ip)[1] == endpoint.ip[1];
            });
        const auto backoff = 30U << (candidate.attempts == 0U ? 0U : candidate.attempts - 1U);
        if (!Occupied(endpoint) && !candidate.peer.has_value() && candidate.attempts < 3U &&
            now - candidate.born < kLifetime && group_peers < 2 &&
            (candidate.attempts == 0U || now - candidate.last_attempt >= backoff)) {
            eligible.push_back(endpoint);
        }
    }
    if (eligible.empty()) {
        return std::nullopt;
    }
    const auto count = static_cast<std::uint64_t>(eligible.size());
    const auto limit = std::numeric_limits<std::uint64_t>::max() -
                       std::numeric_limits<std::uint64_t>::max() % count;
    return *entropy >= limit ? std::nullopt
                             : std::optional<DiscoveryEndpoint>{
                                   eligible[static_cast<std::size_t>(*entropy % count)]};
}

void PeerDiscovery::Attempt(const DiscoveryEndpoint& endpoint,
                            const std::optional<std::uint64_t> id, const std::uint64_t now) noexcept
{
    const auto found = candidates_.find(endpoint);
    if (!Clock(now) || found == candidates_.end() || found->second.attempts >= 3U) {
        return;
    }
    ++found->second.attempts;
    found->second.last_attempt = now;
    found->second.peer = id;
    last_dial_ = now;
}

std::size_t PeerDiscovery::size() const noexcept
{
    return candidates_.size();
}

} // namespace nova::node
