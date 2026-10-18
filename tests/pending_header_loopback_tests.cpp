#include <gtest/gtest.h>

#include "node/peer_service.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace nova::node
{
class PeerServiceTestAccess final
{
  public:
    struct Fault final {
        PeerService* service{};
        std::uint64_t time{10U};
        std::size_t calls{};
    };
    static void Inject(PeerService& service, Fault& fault)
    {
        fault.service = &service;
        service.clock_override_ = &fault.time;
        service.queue_context_ = &fault;
        service.queue_hook_ = [](void* context, const std::uint64_t peer,
                                 const net::FramedMessage& message) noexcept {
            auto& state = *static_cast<Fault*>(context);
            if (++state.calls == 1U)
                return net::TransportError::kQueueLimit;
            return state.service->transport_->Queue(peer, message);
        };
    }
};
} // namespace nova::node

namespace
{
using nova::crypto::Hash256;
using nova::net::Command;
using nova::net::TransportError;
using nova::primitives::Block;

std::string Hex(const Hash256& hash)
{
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result;
    for (const auto byte : hash.bytes()) {
        result += digits[byte >> 4U];
        result += digits[byte & 15U];
    }
    return result;
}

std::uint16_t ReservePort()
{
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0)
        return 0U;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    const bool success = bind(descriptor, reinterpret_cast<const sockaddr*>(&address), size) == 0 &&
                         getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    static_cast<void>(close(descriptor));
    return success ? ntohs(address.sin_port) : 0U;
}

struct Fixture final {
    const nova::consensus::NetworkParams& network = nova::consensus::RegtestNetworkParams();
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("nova-pending-loopback-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<nova::node::RegtestNode> node;
    std::unique_ptr<nova::node::PeerService> service;
    std::unique_ptr<nova::net::TcpTransport> peer;
    std::uint64_t id{};
    bool handshake{};
    bool failed{};
    bool owns_root{};
    std::set<std::string> requested;
    std::uint64_t pong{};
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{40};

    ~Fixture()
    {
        peer.reset();
        service.reset();
        node.reset();
        std::error_code error;
        // Only this fixture's newly-created, fixed-prefix temp directory.
        if (owns_root)
            std::filesystem::remove_all(root, error);
    }

    void Open()
    {
        if (!std::filesystem::create_directory(root))
            throw std::runtime_error{"unique fixture directory unavailable"};
        owns_root = true;
        const auto port = ReservePort();
        if (port == 0U)
            throw std::runtime_error{"loopback port reservation failed"};
        node = nova::node::RegtestNode::Create({"pending-test",
                                                root / "data",
                                                root / "node.log",
                                                port,
                                                28332U,
                                                network.genesis_block.header.time + 10'000U,
                                                {},
                                                nova::consensus::NetworkId::kRegtest});
        if (!node)
            throw std::runtime_error{"node fixture failed"};
        service = nova::node::PeerService::Create(*node);
        if (!service || service->Listen({"127.0.0.1", port}) != TransportError::kNone)
            throw std::runtime_error{"loopback listen failed"};
        const nova::net::P2PParams protocol{network.network_magic,
                                            network.minimum_peer_protocol_version,
                                            network.block_limits.max_serialized_size,
                                            256U,
                                            1000U,
                                            50000U,
                                            101U,
                                            2000U,
                                            network.block_limits.transaction_limits,
                                            network.block_limits};
        const nova::net::ConnectionParams connection{
            protocol, {network.protocol_version, 0U, 1, 987U, "/quota-test/", 0, false},
            5U,       45U,
            60U,      1000U,
            32U,      8U * 1024U * 1024U};
        peer =
            nova::net::TcpTransport::Create({{connection, 0U, 1U}, 65536U, 8U * 1024U * 1024U, 1U});
        const auto connected = peer ? peer->DialNumeric({"127.0.0.1", port}, 1U) : std::nullopt;
        if (!connected)
            throw std::runtime_error{"loopback dial failed"};
        id = *connected;
    }

    bool Pump()
    {
        if (failed || std::chrono::steady_clock::now() >= deadline)
            return false;
        const auto first = service->Pump(1U);
        const auto second = peer->Pump(1U, [this](const std::uint64_t source,
                                                  const nova::net::FramedMessage& message) {
            if (source != id) {
                failed = true;
            } else if (message.command == Command::kVerack) {
                handshake = true;
            } else if (message.command == Command::kGetData) {
                const auto* inventory = std::get_if<nova::net::InventoryMessage>(&message.message);
                if (!inventory || inventory->inventory.size() > 2000U) {
                    failed = true;
                    return;
                }
                for (const auto& item : inventory->inventory) {
                    if (item.type != 2U || requested.size() >= 4100U) {
                        failed = true;
                        return;
                    }
                    requested.insert(Hex(item.hash));
                }
            } else if (message.command == Command::kPong) {
                const auto* nonce = std::get_if<nova::net::NonceMessage>(&message.message);
                if (!nonce)
                    failed = true;
                else
                    pong = nonce->nonce;
            }
        });
        if (first.error != TransportError::kNone || second.error != TransportError::kNone ||
            peer->peer_count() != 1U)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        return !failed;
    }

    template <typename Predicate> bool Until(Predicate predicate)
    {
        for (unsigned int iterations = 0U; iterations < 10000U; ++iterations) {
            if (predicate())
                return !failed;
            if (!Pump())
                return false;
        }
        return false;
    }

    bool Send(const nova::net::FramedMessage& message)
    {
        return peer->Queue(id, message) == TransportError::kNone;
    }

    Block Make(const std::uint32_t serial)
    {
        nova::primitives::Transaction coinbase{
            1,
            {{{{}, 0xFFFF'FFFFU},
              {1U, 1U, static_cast<std::uint8_t>(serial), static_cast<std::uint8_t>(serial >> 8U)},
              0U}},
            {{nova::consensus::GetBlockSubsidy(1U, network.monetary).value(), {0x51U}}},
            0U};
        Block block{{1,
                     network.genesis_hash,
                     {},
                     network.genesis_block.header.time + 600U,
                     network.pow.pow_limit_compact,
                     0U},
                    {coinbase}};
        block.header.merkle_root = nova::primitives::ComputeMerkleRoot(
                                       block.transactions, network.block_limits.transaction_limits)
                                       .value();
        const auto mined = nova::consensus::MineRegtestBlock(block.header, 4096U);
        if (!mined)
            throw std::runtime_error{"bounded fixture mining failed"};
        block.header = mined->header;
        return block;
    }
};

TEST(PendingHeaderLoopback, RuntimeQuotaRejectsOverflowThenRecoversThroughPeerService)
{
    Fixture fixture;
    fixture.Open();
    ASSERT_TRUE(fixture.Until([&] { return fixture.handshake; }));
    std::vector<Block> blocks;
    blocks.reserve(4097U);
    for (std::uint32_t serial = 0U; serial < 4097U; ++serial)
        blocks.push_back(fixture.Make(serial));
    for (std::size_t first = 0U; first < 4096U;) {
        const auto last = std::min(first + 2000U, std::size_t{4096U});
        nova::net::HeadersMessage message;
        for (auto index = first; index < last; ++index)
            message.headers.push_back(blocks.at(index).header);
        ASSERT_TRUE(fixture.Send({Command::kHeaders, message}));
        ASSERT_TRUE(fixture.Until([&] { return fixture.requested.size() == last; }));
        first = last;
    }
    const auto extra = Hex(nova::primitives::ComputeBlockHash(blocks.back().header).value());
    ASSERT_TRUE(
        fixture.Send({Command::kHeaders, nova::net::HeadersMessage{{blocks.back().header}}}));
    ASSERT_TRUE(fixture.Send({Command::kPing, nova::net::NonceMessage{100U}}));
    ASSERT_TRUE(fixture.Until([&] { return fixture.pong == 100U; }));
    // Drain at least another complete pump after the calibrated ping barrier;
    // exact internal saturation/rollback is asserted separately in unit tests.
    ASSERT_TRUE(fixture.Pump());
    EXPECT_EQ(fixture.requested.size(), 4096U);
    EXPECT_FALSE(fixture.requested.contains(extra));
    EXPECT_EQ(fixture.node->height(), 0U);
    ASSERT_TRUE(fixture.Send({Command::kBlock, blocks.front()}));
    ASSERT_TRUE(fixture.Until([&] { return fixture.node->height() == 1U; }));
    ASSERT_TRUE(
        fixture.Send({Command::kHeaders, nova::net::HeadersMessage{{blocks.back().header}}}));
    ASSERT_TRUE(fixture.Until([&] { return fixture.requested.contains(extra); }));
    ASSERT_TRUE(fixture.Send({Command::kBlock, blocks.back()}));
    const auto extra_hash = nova::primitives::ComputeBlockHash(blocks.back().header).value();
    ASSERT_TRUE(fixture.Until(
        [&] { return fixture.node->chain_state().GetHeaderMetadata(extra_hash).has_value(); }));
    EXPECT_EQ(fixture.node->height(), 1U);
    std::cout << "loopback_quota=4096 overflow_unrequested=true validated_body_recovery=true"
                 " accepted_tail="
              << extra << '\n';
}

TEST(PendingHeaderLoopback, FailedEnqueueRecoversWithoutAnotherHeaderOffer)
{
    nova::node::PeerServiceTestAccess::Fault fault;
    Fixture fixture;
    fixture.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
    fixture.Open();
    nova::node::PeerServiceTestAccess::Inject(*fixture.service, fault);
    ASSERT_TRUE(fixture.Until([&] { return fixture.handshake; }));
    const auto block = fixture.Make(1U);
    const auto hash = nova::primitives::ComputeBlockHash(block.header).value();
    const auto before = fixture.node->chain_state().active_tip();
    ASSERT_TRUE(fixture.Send({Command::kHeaders, nova::net::HeadersMessage{{block.header}}}));
    ASSERT_TRUE(fixture.Until([&] { return fault.calls == 1U; }));
    // Calibrated ping/pong shows the connection works while the request was not queued.
    ASSERT_TRUE(fixture.Send({Command::kPing, nova::net::NonceMessage{777U}}));
    ASSERT_TRUE(fixture.Until([&] { return fixture.pong == 777U; }));
    EXPECT_TRUE(fixture.requested.empty());
    EXPECT_EQ(fixture.node->chain_state().active_tip(), before);
    ++fault.time;
    ASSERT_TRUE(fixture.Until([&] { return fixture.requested.contains(Hex(hash)); }));
    EXPECT_EQ(fault.calls, 2U);
    EXPECT_EQ(fixture.service->recovery_counters().retry_admitted, 1U);
    // A successfully queued request whose body is withheld is not retried.
    ++fault.time;
    ASSERT_TRUE(fixture.Send({Command::kPing, nova::net::NonceMessage{778U}}));
    ASSERT_TRUE(fixture.Until([&] { return fixture.pong == 778U; }));
    EXPECT_EQ(fault.calls, 2U);
    EXPECT_EQ(fixture.node->chain_state().active_tip(), before);
    ASSERT_TRUE(fixture.Send({Command::kBlock, block}));
    ASSERT_TRUE(fixture.Until([&] { return fixture.node->chain_state().active_tip() == hash; }));
    fixture.peer.reset();
    fixture.service.reset();
    fixture.node.reset();
    fixture.node =
        nova::node::RegtestNode::Create({"pending-test",
                                         fixture.root / "data",
                                         fixture.root / "node.log",
                                         28333U,
                                         28332U,
                                         fixture.network.genesis_block.header.time + 10'000U,
                                         {},
                                         nova::consensus::NetworkId::kRegtest});
    ASSERT_TRUE(fixture.node);
    EXPECT_EQ(fixture.node->chain_state().active_tip(), hash);
    std::cout << "enqueue_fault=injected failed_then_admitted=true header_offers=1 "
                 "held_body=unchanged validated_body_restart=passed\n";
}

TEST(PendingHeaderLoopback, ActualTransportQueueLimitRecoversAfterDrain)
{
    Fixture fixture;
    fixture.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
    fixture.Open();
    ASSERT_TRUE(fixture.Until([&] { return fixture.handshake; }));
    // Drain any final handshake frame before charging the configured 32 slots.
    ASSERT_TRUE(fixture.Send({Command::kPing, nova::net::NonceMessage{900U}}));
    ASSERT_TRUE(fixture.Until([&] { return fixture.pong == 900U; }));
    for (unsigned int index = 0U; index < 32U; ++index)
        ASSERT_TRUE(fixture.Send({Command::kPing, nova::net::NonceMessage{901U}}));
    EXPECT_FALSE(fixture.Send({Command::kGetData, nova::net::InventoryMessage{}}));
    ASSERT_TRUE(fixture.Until([&] { return fixture.pong == 901U; }));
    ASSERT_TRUE(fixture.Send({Command::kPing, nova::net::NonceMessage{902U}}));
    ASSERT_TRUE(fixture.Until([&] { return fixture.pong == 902U; }));
    std::cout << "actual_transport_queue_limit=observed drain_recovery=passed\n";
}
} // namespace
