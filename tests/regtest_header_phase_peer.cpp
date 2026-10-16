#include "consensus/network_params.hpp"
#include "consensus/pow.hpp"
#include "net/transport.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace
{
std::uint64_t Now()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::string Hex(const std::span<const std::uint8_t> bytes)
{
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result;
    for (const auto byte : bytes) {
        result += digits[byte >> 4U];
        result += digits[byte & 15U];
    }
    return result;
}

std::optional<std::uint16_t> Port(const std::string_view text)
{
    unsigned int value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0U ||
        value > 65535U) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

bool CheckFixture(const nova::primitives::Block& block, const nova::crypto::Hash256& expected)
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto target = nova::consensus::TargetFromCompact(block.header.bits, network.pow);
    nova::primitives::BinaryWriter writer;
    if (block.header.previous_block_id != network.genesis_hash ||
        block.header.time <= network.genesis_block.header.time ||
        block.header.bits != network.genesis_block.header.bits ||
        nova::primitives::ComputeBlockHash(block.header) != expected || !target.target ||
        !nova::consensus::CheckProofOfWork(expected, *target.target) ||
        nova::primitives::CheckBlockStructure(block, network.block_limits) !=
            nova::primitives::BlockStructureError::kNone ||
        nova::primitives::ComputeMerkleRoot(block.transactions,
                                            network.block_limits.transaction_limits) !=
            block.header.merkle_root ||
        !block.Serialize(writer, network.block_limits)) {
        return false;
    }
    const auto decoded = nova::primitives::Block::Deserialize(writer.bytes(), network.block_limits);
    nova::primitives::BinaryWriter roundtrip;
    return decoded && decoded->Serialize(roundtrip, network.block_limits) &&
           std::ranges::equal(writer.bytes(), roundtrip.bytes());
}

bool ExactRequest(const nova::net::InventoryMessage& request, const nova::crypto::Hash256& expected)
{
    return request.inventory.size() == 1U && request.inventory.front().type == 2U &&
           request.inventory.front().hash == expected;
}

std::unique_ptr<nova::net::TcpTransport> Transport(const bool server)
{
    const auto& network = nova::consensus::RegtestNetworkParams();
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
    const nova::net::ConnectionParams connection{protocol,
                                                 {network.protocol_version, 0U,
                                                  static_cast<std::int64_t>(Now()), 44U,
                                                  "/NovaCoin-header-phase-test/", 0, false},
                                                 5U,
                                                 40U,
                                                 40U,
                                                 100U,
                                                 16U,
                                                 4U * 1024U * 1024U};
    return nova::net::TcpTransport::Create(
        {{connection, server ? 1U : 0U, server ? 0U : 1U}, 64U * 1024U, 4U * 1024U * 1024U, 1U});
}

std::optional<nova::primitives::Block> Fetch(const std::uint16_t port,
                                             const nova::crypto::Hash256& expected)
{
    auto transport = Transport(false);
    if (!transport || !transport->Dial({"127.0.0.1", port}, Now())) {
        return std::nullopt;
    }
    std::optional<nova::primitives::Block> fixture;
    bool requested{};
    bool failed{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    for (unsigned int pumps = 0; pumps < 2000U && std::chrono::steady_clock::now() < deadline;
         ++pumps) {
        const auto result = transport->Pump(
            Now(), [&](const std::uint64_t peer, const nova::net::FramedMessage& message) {
                if (message.command == nova::net::Command::kVerack && !requested) {
                    requested = true;
                    const nova::net::InventoryMessage request{{{2U, expected}}};
                    failed = transport->Queue(peer, {nova::net::Command::kGetData, request}) !=
                             nova::net::TransportError::kNone;
                    std::cout << "source_getdata_sent=true\n";
                } else if (message.command == nova::net::Command::kBlock) {
                    const auto* block = std::get_if<nova::primitives::Block>(&message.message);
                    if (!requested || fixture || !block || !CheckFixture(*block, expected)) {
                        failed = true;
                    } else {
                        fixture = *block;
                    }
                }
            });
        if (failed || result.error != nova::net::TransportError::kNone) {
            return std::nullopt;
        }
        if (fixture) {
            // Destroy the source socket before starting the target listener.
            transport->Close();
            std::cout << "source_connection_closed=true\n";
            return fixture;
        }
        if (transport->peer_count() == 0U) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return std::nullopt;
}

bool Marker(const std::filesystem::path& directory, const char* name)
{
    const auto path = directory / name;
    if (std::filesystem::exists(path)) {
        return false;
    }
    std::ofstream file{path, std::ios::binary};
    file << "ready=true\n";
    file.close();
    return static_cast<bool>(file);
}

bool Serve(const nova::primitives::Block& block, const std::uint16_t port,
           const std::filesystem::path& directory)
{
    auto transport = Transport(true);
    if (!transport || transport->Listen({"127.0.0.1", port}) != nova::net::TransportError::kNone ||
        !Marker(directory, "listening.txt")) {
        return false;
    }
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto expected = nova::primitives::ComputeBlockHash(block.header);
    bool handshake{};
    bool headers_sent{};
    bool requested{};
    bool body_queued{};
    bool flushed{};
    bool failed{};
    std::uint64_t target_peer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    for (unsigned int pumps = 0; pumps < 6000U && std::chrono::steady_clock::now() < deadline;
         ++pumps) {
        const bool previously_queued = body_queued;
        const auto result = transport->Pump(Now(), [&](const std::uint64_t peer,
                                                       const nova::net::FramedMessage& message) {
            if (message.command == nova::net::Command::kVerack) {
                if (handshake) {
                    failed = true;
                }
                handshake = true;
                target_peer = peer;
                std::cout << "target_verack=true\n";
            } else if (message.command == nova::net::Command::kGetHeaders) {
                const auto* request = std::get_if<nova::net::GetHeadersMessage>(&message.message);
                if (!handshake || peer != target_peer || headers_sent || !request ||
                    request->version != network.protocol_version ||
                    request->locator_hashes.size() != 1U ||
                    request->locator_hashes.front() != network.genesis_hash ||
                    request->stop_hash != nova::crypto::Hash256{}) {
                    failed = true;
                    return;
                }
                std::cout << "target_getheaders=genesis_locator\n";
                const nova::net::HeadersMessage response{{block.header}};
                failed = transport->Queue(peer, {nova::net::Command::kHeaders, response}) !=
                         nova::net::TransportError::kNone;
                headers_sent = !failed;
                std::cout << "helper_headers_queued=1\n";
            } else if (message.command == nova::net::Command::kGetData) {
                const auto* request = std::get_if<nova::net::InventoryMessage>(&message.message);
                if (!headers_sent || requested || peer != target_peer || !request ||
                    !ExactRequest(*request, *expected)) {
                    failed = true;
                    return;
                }
                requested = true;
                std::cout << "target_getdata=exact_block\nbody_withheld=true\n";
                failed = !Marker(directory, "requested.txt");
            } else if (message.command != nova::net::Command::kVersion) {
                // No inventory/body/transaction is expected from the target
                // in this deliberately quiet, genesis-height phase.
                failed = true;
            }
        });
        if (failed || result.error != nova::net::TransportError::kNone ||
            (handshake && transport->peer_count() != 1U)) {
            return false;
        }
        const bool release = std::filesystem::exists(directory / "release.txt");
        const bool done = std::filesystem::exists(directory / "done.txt");
        if ((release && !requested) || (done && !flushed)) {
            return false;
        }
        if (release && !body_queued) {
            std::cout << "release_observed=true\n";
            if (transport->Queue(target_peer, {nova::net::Command::kBlock, block}) !=
                nova::net::TransportError::kNone) {
                return false;
            }
            body_queued = true;
            std::cout << "helper_block_queued=1\n";
        }
        if (previously_queued && !flushed) {
            flushed = true;
            if (!Marker(directory, "flushed.txt")) {
                return false;
            }
            std::cout << "body_flush_pump=passed\n";
        }
        if (done) {
            std::cout << "target_confirmation_observed=true\n";
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
}

bool SelfTest()
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    auto block = network.genesis_block;
    block.header.previous_block_id = network.genesis_hash;
    block.header.time += 1U;
    const auto mined = nova::consensus::MineRegtestBlock(block.header, 4096U);
    if (!mined) {
        return false;
    }
    block.header = mined->header;
    if (!CheckFixture(block, mined->block_hash) || Port("0") || Port("65536") || Port("1x") ||
        Port("1") != 1U || Port("65535") != 65535U ||
        !ExactRequest({{{2U, mined->block_hash}}}, mined->block_hash) ||
        ExactRequest({{{1U, mined->block_hash}}}, mined->block_hash) ||
        ExactRequest({{{2U, network.genesis_hash}}}, mined->block_hash) ||
        ExactRequest({}, mined->block_hash) ||
        ExactRequest({{{2U, mined->block_hash}, {2U, mined->block_hash}}}, mined->block_hash)) {
        return false;
    }
    auto bad = block;
    bad.header.previous_block_id = {};
    if (CheckFixture(bad, mined->block_hash)) {
        return false;
    }
    bad = block;
    bad.transactions.clear();
    return !CheckFixture(bad, mined->block_hash) &&
           !CheckFixture(network.genesis_block, network.genesis_hash);
}
} // namespace

int main(const int argc, const char* const* argv)
{
    try {
        if (argc == 2 && std::string_view{argv[1]} == "--self-test") {
            if (!SelfTest()) {
                return 1;
            }
            std::cout << "header_phase_self_test=passed\n";
            return 0;
        }
        if (argc != 5) {
            return 2;
        }
        const auto source_port = Port(argv[1]);
        const auto listen_port = Port(argv[2]);
        const auto expected = nova::crypto::Hash256::FromHex(argv[3]);
        const std::filesystem::path directory{argv[4]};
        if (!source_port || !listen_port || source_port == listen_port || !expected ||
            !std::filesystem::is_directory(directory) || !std::filesystem::is_empty(directory)) {
            return 2;
        }
        std::cout << std::unitbuf;
        const auto fixture = Fetch(*source_port, *expected);
        if (!fixture) {
            return 1;
        }
        nova::primitives::BinaryWriter writer;
        if (!fixture->Serialize(writer, nova::consensus::RegtestNetworkParams().block_limits)) {
            return 1;
        }
        std::cout << "fixture_hash=" << Hex(expected->bytes()) << '\n'
                  << "fixture_hex=" << Hex(writer.bytes()) << '\n'
                  << "canonical_fixture=passed\n";
        if (!Serve(*fixture, *listen_port, directory)) {
            return 1;
        }
        return 0;
    } catch (...) {
        std::cerr << "bounded header phase failed\n";
        return 1;
    }
}
