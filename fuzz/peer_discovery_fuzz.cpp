#include "node/peer_discovery.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    nova::node::PeerDiscovery store{11000U};
    const auto target = nova::node::DiscoveryEndpoint{{127U, 0U, 0U, 1U}, 11001U};
    std::uint64_t now{};
    for (std::size_t offset = 0U; offset + 19U <= size && offset < 4096U; offset += 19U) {
        const auto id = static_cast<std::uint64_t>(offset) + 1U;
        const std::vector<nova::net::TransportPeer> peers{
            {id, {nova::net::ConnectionDirection::kOutbound, true}, target.Numeric(), target.ip}};
        store.Update(peers, now);
        static_cast<void>(store.Requests(now));
        nova::net::NetworkAddress address{};
        std::copy_n(data + offset, 16U, address.address.begin());
        address.port = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data[offset + 16U]) << 8U) | data[offset + 17U]);
        if ((data[offset] & 1U) != 0U) {
            address =
                nova::node::DiscoveryEndpoint{
                    {127U, 0U, 0U, 1U}, static_cast<std::uint16_t>(12000U + address.port % 10000U)}
                    .Wire();
        }
        store.Receive(id, {{address}}, now);
        const auto next = store.Next(now, data[offset + 18U]);
        if (next.has_value()) {
            store.Attempt(*next, std::nullopt, now);
        }
        now += data[offset + 18U];
        if (store.size() > 256U) {
            std::abort();
        }
    }
    return 0;
}
