#include "consensus/network_params.hpp"
#include "consensus/pow.hpp"
#include "net/transport.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace
{
std::string Hex(const nova::crypto::Hash256& hash)
{
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result;
    for (const auto byte : hash.bytes()) {
        result += digits[byte >> 4U];
        result += digits[byte & 15U];
    }
    return result;
}

std::optional<nova::primitives::Block> InvalidPow(nova::primitives::Block block)
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto target = nova::consensus::TargetFromCompact(block.header.bits, network.pow);
    const auto original = nova::primitives::ComputeBlockHash(block.header);
    if (!target.target || !original ||
        !nova::consensus::CheckProofOfWork(*original, *target.target)) {
        return std::nullopt;
    }
    for (std::uint32_t attempt = 0; attempt < 4096U; ++attempt) {
        // Unsigned nonce wrap is explicit and deterministic.
        block.header.nonce += 1U;
        const auto hash = nova::primitives::ComputeBlockHash(block.header);
        if (hash && *hash != *original &&
            !nova::consensus::CheckProofOfWork(*hash, *target.target)) {
            nova::primitives::BinaryWriter writer;
            if (nova::primitives::CheckBlockStructure(block, network.block_limits) !=
                    nova::primitives::BlockStructureError::kNone ||
                !block.Serialize(writer, network.block_limits)) {
                return std::nullopt;
            }
            const auto decoded =
                nova::primitives::Block::Deserialize(writer.bytes(), network.block_limits);
            if (!decoded || nova::primitives::ComputeBlockHash(decoded->header) != hash) {
                return std::nullopt;
            }
            return block;
        }
    }
    return std::nullopt;
}

std::uint64_t Now()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}
} // namespace

int main(const int argc, const char* const* argv)
{
    try {
        const auto& network = nova::consensus::RegtestNetworkParams();
        if (argc == 2 && std::string_view{argv[1]} == "--self-test") {
            const auto first = InvalidPow(network.genesis_block);
            const auto second = InvalidPow(network.genesis_block);
            if (!first || !second || first->header.nonce != second->header.nonce ||
                InvalidPow(*first).has_value() ||
                InvalidPow(nova::primitives::Block{}).has_value()) {
                return 1;
            }
            std::cout << "deterministic_fixture_self_test=passed\n";
            return 0;
        }
        if (argc != 3) {
            std::cerr << "usage: regtest_invalid_block_peer loopback-port expected-block-hash\n";
            return 2;
        }
        unsigned int port{};
        const std::string_view port_text{argv[1]};
        const auto parsed =
            std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        const auto expected = nova::crypto::Hash256::FromHex(argv[2]);
        if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() ||
            port == 0U || port > 65535U || !expected) {
            return 2;
        }
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
        const auto now = Now();
        const nova::net::ConnectionParams connection{protocol,
                                                     {network.protocol_version, 0U,
                                                      static_cast<std::int64_t>(now), 42U,
                                                      "/NovaCoin-invalid-block-test/", 0, false},
                                                     5U,
                                                     10U,
                                                     10U,
                                                     100U,
                                                     16U,
                                                     4U * 1024U * 1024U};
        auto transport = nova::net::TcpTransport::Create(
            {{connection, 0U, 1U}, 64U * 1024U, 4U * 1024U * 1024U, 1U});
        if (!transport) {
            return 1;
        }
        const auto peer = transport->Dial({"127.0.0.1", static_cast<std::uint16_t>(port)}, now);
        if (!peer) {
            return 1;
        }
        bool requested{};
        bool submitted{};
        bool failed{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        for (unsigned int pumps = 0; pumps < 2000U && std::chrono::steady_clock::now() < deadline;
             ++pumps) {
            const auto result = transport->Pump(
                Now(), [&](const std::uint64_t id, const nova::net::FramedMessage& message) {
                    if (message.command == nova::net::Command::kVerack && !requested) {
                        requested = true;
                        const nova::net::InventoryMessage request{{{2U, *expected}}};
                        failed = transport->Queue(id, {nova::net::Command::kGetData, request}) !=
                                 nova::net::TransportError::kNone;
                    } else if (message.command == nova::net::Command::kBlock && !submitted) {
                        const auto* block = std::get_if<nova::primitives::Block>(&message.message);
                        if (!block ||
                            nova::primitives::ComputeBlockHash(block->header) != expected) {
                            failed = true;
                            return;
                        }
                        const auto invalid = InvalidPow(*block);
                        if (!invalid) {
                            failed = true;
                            return;
                        }
                        failed = transport->Queue(id, {nova::net::Command::kBlock, *invalid}) !=
                                 nova::net::TransportError::kNone;
                        submitted = !failed;
                        std::cout << "control_hash=" << Hex(*expected) << '\n'
                                  << "invalid_hash="
                                  << Hex(*nova::primitives::ComputeBlockHash(invalid->header))
                                  << '\n'
                                  << "invalid_nonce=" << invalid->header.nonce << '\n'
                                  << "structure_roundtrip=passed\npow=invalid\n";
                    }
                });
            if (failed || result.error != nova::net::TransportError::kNone) {
                return 1;
            }
            if (transport->peer_count() == 0U) {
                if (requested && submitted) {
                    std::cout << "peer_closed_after_submission=true\n";
                    return 0;
                }
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        std::cerr << "bounded peer test timed out\n";
        return 1;
    } catch (...) {
        std::cerr << "peer test failed\n";
        return 1;
    }
}
