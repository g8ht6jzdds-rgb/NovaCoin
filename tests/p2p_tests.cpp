#include <gtest/gtest.h>

#include "consensus/network_params.hpp"
#include "net/p2p.hpp"
#include "net/transport.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace
{

nova::net::P2PParams TestProtocolParams()
{
    constexpr nova::primitives::TransactionLimits kTransactionLimits{21'000'000LL * 100'000'000LL,
                                                                     100'000U, 8U, 8U, 128U};
    constexpr nova::primitives::BlockLimits kBlockLimits{kTransactionLimits, 100'000U, 8U, 64U};
    return {0xDAB5'BFFAU, 1, 100'000U, 256U, 16U, 32U, 16U, 16U, kTransactionLimits, kBlockLimits};
}

nova::net::VersionMessage Version()
{
    return {1, 0U, 1'000, 7U, "/nova:test/", 0, true};
}

nova::net::ConnectionParams TestConnectionParams()
{
    return {TestProtocolParams(), Version(), 100U, 200U, 10U, 8U, 8U, 200'000U};
}

TEST(P2PFraming, RoundTripsCanonicalVersionAcrossFragmentedInput)
{
    const nova::net::FramedMessage message{nova::net::Command::kVersion, Version()};
    const auto serialized = nova::net::SerializeMessage(message, TestProtocolParams());
    ASSERT_TRUE(serialized.has_value());
    nova::net::MessageParser parser{TestProtocolParams()};
    const auto partial = parser.PushBytes(std::span<const std::uint8_t>{*serialized}.first(11U));
    EXPECT_EQ(partial.error, nova::net::P2PError::kNone);
    EXPECT_TRUE(partial.messages.empty());
    const auto complete = parser.PushBytes(std::span<const std::uint8_t>{*serialized}.subspan(11U));
    ASSERT_EQ(complete.error, nova::net::P2PError::kNone);
    ASSERT_EQ(complete.messages.size(), 1U);
    EXPECT_EQ(complete.messages.front().command, nova::net::Command::kVersion);
    const auto* version =
        std::get_if<nova::net::VersionMessage>(&complete.messages.front().message);
    ASSERT_NE(version, nullptr);
    EXPECT_EQ(version->user_agent, "/nova:test/");
    EXPECT_TRUE(version->relay);
}

TEST(P2PFraming, RejectsPeerBelowConfiguredMinimumProtocolVersion)
{
    auto outdated = Version();
    outdated.version = 0;
    const nova::net::FramedMessage message{nova::net::Command::kVersion, outdated};
    const auto serialized = nova::net::SerializeMessage(message, TestProtocolParams());
    ASSERT_TRUE(serialized.has_value());

    nova::net::MessageParser parser{TestProtocolParams()};
    EXPECT_EQ(parser.PushBytes(*serialized).error, nova::net::P2PError::kMalformedPayload);
}

TEST(P2PFraming, RejectsEveryOtherNetworkMagicBeforeDispatch)
{
    const auto networks = {&nova::consensus::RegtestNetworkParams(),
                           &nova::consensus::TestnetNetworkParams(),
                           &nova::consensus::MainnetNetworkParams()};
    for (const auto* source : networks) {
        auto source_params = TestProtocolParams();
        source_params.network_magic = source->network_magic;
        const auto bytes = nova::net::SerializeMessage(
            {nova::net::Command::kPing, nova::net::NonceMessage{9U}}, source_params);
        ASSERT_TRUE(bytes.has_value());
        for (const auto* target : networks) {
            auto target_params = TestProtocolParams();
            target_params.network_magic = target->network_magic;
            nova::net::MessageParser parser{target_params};
            const auto result = parser.PushBytes(*bytes);
            EXPECT_EQ(result.error, source->id == target->id
                                        ? nova::net::P2PError::kNone
                                        : nova::net::P2PError::kUnexpectedMagic);
            EXPECT_EQ(result.messages.size(), source->id == target->id ? 1U : 0U);
        }
    }
}

TEST(P2PFraming, RejectsBadChecksumMagicAndDeclaredOversize)
{
    const nova::net::FramedMessage message{nova::net::Command::kPing, nova::net::NonceMessage{9U}};
    const auto serialized = nova::net::SerializeMessage(message, TestProtocolParams());
    ASSERT_TRUE(serialized.has_value());
    auto bad_checksum = *serialized;
    bad_checksum.at(20U) ^= 1U;
    nova::net::MessageParser checksum_parser{TestProtocolParams()};
    EXPECT_EQ(checksum_parser.PushBytes(bad_checksum).error,
              nova::net::P2PError::kChecksumMismatch);

    auto bad_magic = *serialized;
    bad_magic.at(0U) ^= 1U;
    nova::net::MessageParser magic_parser{TestProtocolParams()};
    EXPECT_EQ(magic_parser.PushBytes(bad_magic).error, nova::net::P2PError::kUnexpectedMagic);

    auto oversized = *serialized;
    oversized.at(16U) = 0xFFU;
    oversized.at(17U) = 0xFFU;
    oversized.at(18U) = 0xFFU;
    oversized.at(19U) = 0x7FU;
    nova::net::MessageParser length_parser{TestProtocolParams()};
    EXPECT_EQ(length_parser.PushBytes(oversized).error, nova::net::P2PError::kPayloadTooLarge);
}

TEST(P2PMessages, RoundTripsBoundedAddrInventoryGetHeadersAndHeaders)
{
    const auto parameters = TestProtocolParams();
    const nova::crypto::Hash256 hash{};
    const std::array<nova::net::FramedMessage, 4U> messages{
        nova::net::FramedMessage{
            nova::net::Command::kAddr,
            nova::net::AddressMessage{{nova::net::NetworkAddress{0U, {}, 8333U}}}},
        nova::net::FramedMessage{
            nova::net::Command::kInv,
            nova::net::InventoryMessage{{nova::net::InventoryVector{1U, hash}}}},
        nova::net::FramedMessage{nova::net::Command::kGetHeaders,
                                 nova::net::GetHeadersMessage{1, {hash}, hash}},
        nova::net::FramedMessage{nova::net::Command::kHeaders,
                                 nova::net::HeadersMessage{{nova::primitives::BlockHeader{}}}},
    };
    for (const auto& message : messages) {
        const auto serialized = nova::net::SerializeMessage(message, parameters);
        ASSERT_TRUE(serialized.has_value());
        nova::net::MessageParser parser{parameters};
        const auto parsed = parser.PushBytes(*serialized);
        ASSERT_EQ(parsed.error, nova::net::P2PError::kNone);
        ASSERT_EQ(parsed.messages.size(), 1U);
        EXPECT_EQ(parsed.messages.front().command, message.command);
    }
}

TEST(PeerManager, EnforcesHandshakeQueueAndConnectionLimits)
{
    nova::net::PeerManager manager{{TestConnectionParams(), 1U, 1U}};
    const auto inbound = manager.AddInbound(0U);
    ASSERT_TRUE(inbound.has_value());
    EXPECT_FALSE(manager.AddInbound(0U).has_value());
    const auto outbound = manager.TakeOutbound(*inbound);
    ASSERT_EQ(outbound.size(), 1U);

    const auto version = nova::net::SerializeMessage({nova::net::Command::kVersion, Version()},
                                                     TestProtocolParams());
    const auto verack = nova::net::SerializeMessage({nova::net::Command::kVerack, std::monostate{}},
                                                    TestProtocolParams());
    ASSERT_TRUE(version.has_value());
    ASSERT_TRUE(verack.has_value());
    EXPECT_EQ(manager.Receive(*inbound, *version, 1U).error, nova::net::P2PError::kNone);
    EXPECT_EQ(manager.Receive(*inbound, *verack, 2U).error, nova::net::P2PError::kNone);
    EXPECT_EQ(manager.peer_count(), 1U);
    manager.Tick(300U);
    EXPECT_EQ(manager.peer_count(), 0U);
}

TEST(P2PMessages, GetAddrRequiresEmptyPayloadAndAddrRejectsNoncanonicalCounts)
{
    const auto params = TestProtocolParams();
    EXPECT_TRUE(
        nova::net::DeserializeMessage(nova::net::Command::kGetAddr, {}, params).has_value());
    const std::array<std::uint8_t, 1U> trailing{0U};
    EXPECT_FALSE(
        nova::net::DeserializeMessage(nova::net::Command::kGetAddr, trailing, params).has_value());
    const std::array<std::uint8_t, 3U> noncanonical{0xFDU, 1U, 0U};
    EXPECT_FALSE(
        nova::net::DeserializeMessage(nova::net::Command::kAddr, noncanonical, params).has_value());
    nova::net::PeerManager manager{{TestConnectionParams(), 1U, 1U}};
    const auto peer = manager.AddInbound(0U);
    ASSERT_TRUE(peer.has_value());
    const auto message =
        nova::net::SerializeMessage({nova::net::Command::kGetAddr, std::monostate{}}, params);
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(manager.Receive(*peer, *message, 1U).error, nova::net::P2PError::kHandshakeViolation);
    EXPECT_FALSE(manager.Status(*peer).has_value());
}

TEST(TcpTransport, RejectsUnsafeBufferAndAcceptLimitsBeforeOpeningSockets)
{
    const nova::net::PeerManagerParams peers{TestConnectionParams(), 1U, 1U};
    EXPECT_EQ(nova::net::TcpTransport::Create({peers, 0U, 1024U, 1U}), nullptr);
    EXPECT_EQ(nova::net::TcpTransport::Create({peers, 1024U, 0U, 1U}), nullptr);
    EXPECT_EQ(nova::net::TcpTransport::Create({peers, 1024U, 1024U, 0U}), nullptr);
}

TEST(TcpTransport, NumericDialRejectsHostnameAndExhaustedCapacity)
{
    auto transport =
        nova::net::TcpTransport::Create({{TestConnectionParams(), 1U, 1U}, 1024U, 1024U, 1U});
    ASSERT_NE(transport, nullptr);
    EXPECT_FALSE(transport->DialNumeric({"localhost", 18444U}, 0U).has_value());
    EXPECT_EQ(transport->peer_count(), 0U);
    auto full =
        nova::net::TcpTransport::Create({{TestConnectionParams(), 1U, 0U}, 1024U, 1024U, 1U});
    ASSERT_NE(full, nullptr);
    EXPECT_FALSE(full->DialNumeric({"127.0.0.1", 18444U}, 0U).has_value());
    EXPECT_EQ(full->peer_count(), 0U);
}

} // namespace
