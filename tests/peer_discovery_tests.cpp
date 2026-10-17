#include <gtest/gtest.h>

#include "node/peer_discovery.hpp"

#include <limits>

namespace
{
using nova::node::DiscoveryEndpoint;
using nova::node::PeerDiscovery;

DiscoveryEndpoint Endpoint(const std::uint16_t port)
{
    return {{127U, 0U, 0U, 1U}, port};
}

nova::net::TransportPeer Peer(const std::uint64_t id, const std::uint16_t port,
                              const bool outbound = true, const bool established = true)
{
    return {id,
            {outbound ? nova::net::ConnectionDirection::kOutbound
                      : nova::net::ConnectionDirection::kInbound,
             established},
            outbound ? std::optional<nova::net::TcpEndpoint>{Endpoint(port).Numeric()}
                     : std::nullopt,
            Endpoint(port).ip};
}

void Learn(PeerDiscovery& store, const nova::net::AddressMessage& addresses,
           const std::uint64_t now = 0U)
{
    store.Update({Peer(1U, 11001U)}, now);
    ASSERT_EQ(store.Requests(now), std::vector<std::uint64_t>{1U});
    store.Receive(1U, addresses, now);
}

TEST(PeerDiscovery, RequiresCanonicalMappedLoopbackAndNonzeroPort)
{
    const auto valid = Endpoint(65535U).Wire();
    ASSERT_EQ(DiscoveryEndpoint::FromWire(valid), Endpoint(65535U));
    for (std::size_t index = 0U; index < 16U; ++index) {
        auto invalid = valid;
        invalid.address[index] ^= 1U;
        EXPECT_FALSE(DiscoveryEndpoint::FromWire(invalid).has_value());
    }
    auto invalid = valid;
    invalid.port = 0U;
    EXPECT_FALSE(DiscoveryEndpoint::FromWire(invalid).has_value());
    for (const auto* host : {"localhost", "127.1", "127.000.0.1", "127.0.0.2", "::1", "10.0.0.1",
                             "169.254.169.254", "8.8.8.8", "224.0.0.1"}) {
        EXPECT_FALSE(DiscoveryEndpoint::FromNumeric({host, 11002U}).has_value());
    }
}

TEST(PeerDiscovery, RejectsUnsolicitedLateAndRepeatedResponses)
{
    PeerDiscovery store{11000U};
    const nova::net::AddressMessage message{{Endpoint(11002U).Wire()}};
    store.Update({Peer(1U, 11001U)}, 0U);
    store.Receive(1U, message, 0U);
    EXPECT_EQ(store.size(), 0U);
    ASSERT_EQ(store.Requests(0U).size(), 1U);
    store.Receive(1U, message, 10U);
    EXPECT_EQ(store.size(), 0U);
    store.Receive(1U, message, 11U);
    EXPECT_EQ(store.size(), 0U);
}

TEST(PeerDiscovery, DoesNotLearnBeforeHandshakeOrAfterQueueFailure)
{
    PeerDiscovery store{11000U};
    store.Update({Peer(1U, 11001U, true, false)}, 0U);
    EXPECT_TRUE(store.Requests(0U).empty());
    store.Update({Peer(1U, 11001U)}, 1U);
    ASSERT_EQ(store.Requests(1U).size(), 1U);
    store.RequestFailed(1U);
    store.Receive(1U, {{Endpoint(11002U).Wire()}}, 1U);
    EXPECT_EQ(store.size(), 0U);
}

TEST(PeerDiscovery, ExcludesSelfAndKnownEndpointsAndDeduplicates)
{
    PeerDiscovery store{11000U};
    Learn(store, {{Endpoint(11000U).Wire(), Endpoint(11001U).Wire(), Endpoint(11002U).Wire(),
                   Endpoint(11002U).Wire()}});
    EXPECT_EQ(store.size(), 1U);
    EXPECT_EQ(store.Next(0U, 0U), Endpoint(11002U));
    EXPECT_FALSE(store.Next(0U, std::nullopt).has_value());
}

TEST(PeerDiscovery, SourceQuotaAndBatchAreBoundedAcrossReconnects)
{
    PeerDiscovery store{11000U};
    nova::net::AddressMessage message;
    for (std::uint16_t port = 12000U; port < 13000U; ++port) {
        message.addresses.push_back(Endpoint(port).Wire());
    }
    Learn(store, message);
    EXPECT_EQ(store.size(), 16U);
    store.Update({Peer(2U, 11001U)}, 1U);
    EXPECT_TRUE(store.Requests(1U).empty());
    EXPECT_EQ(store.Requests(60U).size(), 1U);
    store.Receive(2U, {{Endpoint(14000U).Wire()}}, 60U);
    EXPECT_EQ(store.size(), 16U);
}

TEST(PeerDiscovery, DuplicateGossipDoesNotExtendLifetime)
{
    PeerDiscovery store{11000U};
    Learn(store, {{Endpoint(11002U).Wire()}});
    store.Update({Peer(2U, 11001U)}, 60U);
    ASSERT_EQ(store.Requests(60U).size(), 1U);
    store.Receive(2U, {{Endpoint(11002U).Wire()}}, 60U);
    store.Update({Peer(2U, 11001U)}, 1800U);
    EXPECT_EQ(store.size(), 0U);
}

TEST(PeerDiscovery, FixedEntropySelectionAndClockRollbackFailClosed)
{
    PeerDiscovery store{11000U};
    Learn(store, {{Endpoint(11002U).Wire(), Endpoint(11003U).Wire()}}, 10U);
    EXPECT_EQ(store.Next(10U, 0U), Endpoint(11002U));
    EXPECT_EQ(store.Next(10U, 1U), Endpoint(11003U));
    EXPECT_FALSE(store.Next(10U, std::numeric_limits<std::uint64_t>::max()).has_value());
    EXPECT_FALSE(store.Next(9U, 0U).has_value());
    EXPECT_TRUE(store.Requests(9U).empty());
    EXPECT_EQ(store.Next(11U, 0U), Endpoint(11002U));
}

TEST(PeerDiscovery, BackoffAndAttemptCountBoundFailures)
{
    PeerDiscovery store{11000U};
    Learn(store, {{Endpoint(11002U).Wire()}});
    store.Attempt(Endpoint(11002U), std::nullopt, 0U);
    EXPECT_FALSE(store.Next(29U, 0U).has_value());
    ASSERT_TRUE(store.Next(30U, 0U).has_value());
    store.Attempt(Endpoint(11002U), std::nullopt, 30U);
    EXPECT_FALSE(store.Next(89U, 0U).has_value());
    ASSERT_TRUE(store.Next(90U, 0U).has_value());
    store.Attempt(Endpoint(11002U), std::nullopt, 90U);
    EXPECT_FALSE(store.Next(300U, 0U).has_value());
}

TEST(PeerDiscovery, PendingConnectedAndGroupLimitsPreventDuplicateDials)
{
    PeerDiscovery store{11000U};
    Learn(store, {{Endpoint(11002U).Wire(), Endpoint(11003U).Wire()}});
    store.Attempt(Endpoint(11002U), 2U, 0U);
    store.Update({Peer(1U, 11001U), Peer(2U, 11002U, true, false)}, 1U);
    EXPECT_FALSE(store.Next(1U, 0U).has_value());
    store.Update({Peer(1U, 11001U), Peer(2U, 11002U)}, 2U);
    EXPECT_FALSE(store.Next(2U, 0U).has_value());
    store.Update({Peer(1U, 11001U)}, 3U);
    EXPECT_EQ(store.Next(3U, 0U), Endpoint(11003U));
    store.Update({Peer(1U, 11001U)}, 1800U);
    EXPECT_EQ(store.size(), 0U);
}

TEST(PeerDiscovery, RepliesContainOnlyEstablishedOutboundTargets)
{
    PeerDiscovery store{11000U};
    store.Update({Peer(1U, 11001U), Peer(2U, 11002U), Peer(3U, 11003U, false),
                  Peer(4U, 11004U, true, false)},
                 0U);
    const auto reply = store.Reply(1U, 0U);
    ASSERT_TRUE(reply.has_value());
    ASSERT_EQ(reply->addresses.size(), 1U);
    EXPECT_EQ(DiscoveryEndpoint::FromWire(reply->addresses[0]), Endpoint(11002U));
    EXPECT_FALSE(store.Reply(1U, 1U).has_value());
    store.Update({Peer(5U, 11001U), Peer(2U, 11002U)}, 2U);
    EXPECT_FALSE(store.Reply(5U, 2U).has_value());
    EXPECT_TRUE(store.Reply(5U, 60U).has_value());
}

TEST(PeerDiscovery, NoOutboundCapacityOrEntropyMeansNoSelection)
{
    PeerDiscovery store{11000U};
    Learn(store, {{Endpoint(11002U).Wire()}});
    EXPECT_FALSE(store.Next(0U, std::nullopt).has_value());
    std::vector<nova::net::TransportPeer> full;
    for (std::uint16_t index = 1U; index <= 14U; ++index) {
        full.push_back(Peer(index, static_cast<std::uint16_t>(13000U + index)));
    }
    store.Update(full, 1U);
    EXPECT_FALSE(store.Next(1U, 0U).has_value());
}

TEST(PeerDiscovery, ExplicitHostnamePeersAreDeduplicatedByObservedNumericTarget)
{
    PeerDiscovery store{11000U};
    Learn(store, {{Endpoint(11002U).Wire()}});
    auto known = Peer(2U, 11002U);
    known.outbound_target->host = "localhost";
    store.Update({Peer(1U, 11001U), known}, 1U);
    EXPECT_FALSE(store.Next(1U, 0U).has_value());
    known.status.established = false;
    known.source_ipv4.reset();
    store.Update({known}, 2U);
    EXPECT_FALSE(store.Next(2U, 0U).has_value());
}

TEST(PeerDiscovery, EmptyResponseConsumesRequestAndUnknownSenderCannotAdmit)
{
    PeerDiscovery store{11000U};
    store.Update({Peer(1U, 11001U)}, 0U);
    ASSERT_EQ(store.Requests(0U).size(), 1U);
    store.Receive(2U, {{Endpoint(11002U).Wire()}}, 0U);
    EXPECT_EQ(store.size(), 0U);
    store.Receive(1U, {}, 0U);
    store.Receive(1U, {{Endpoint(11002U).Wire()}}, 1U);
    EXPECT_EQ(store.size(), 0U);
    EXPECT_TRUE(store.Requests(60U).empty());
}

TEST(PeerDiscovery, LifetimeDoesNotEvictLivePeerBookkeeping)
{
    PeerDiscovery store{11000U};
    Learn(store, {{Endpoint(11002U).Wire()}});
    store.Attempt(Endpoint(11002U), 2U, 0U);
    store.Update({Peer(1U, 11001U), Peer(2U, 11002U)}, 1800U);
    EXPECT_EQ(store.size(), 1U);
    EXPECT_FALSE(store.Next(1800U, 0U).has_value());
    store.Update({Peer(1U, 11001U)}, 1801U);
    EXPECT_EQ(store.size(), 0U);
}
} // namespace
