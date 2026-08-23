#include <gtest/gtest.h>

#include "explorer/explorer.hpp"
#include "explorer/http.hpp"
#include "explorer/http_server.hpp"
#include "node/regtest_node.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{

using nova::crypto::Hash160;
using nova::explorer::ExplorerBlock;
using nova::explorer::ExplorerError;
using nova::explorer::ExplorerIndex;
using nova::explorer::ExplorerSnapshot;
using nova::primitives::Block;
using nova::primitives::OutPoint;
using nova::primitives::Transaction;

std::vector<std::uint8_t> P2pkhScript(const std::uint8_t value)
{
    std::vector<std::uint8_t> script{0x76U, 0xA9U, 0x14U};
    script.insert(script.end(), 20U, value);
    script.push_back(0x88U);
    script.push_back(0xACU);
    return script;
}

Transaction Coinbase(const nova::primitives::Amount value, const std::uint8_t address)
{
    return Transaction{1,
                       {{OutPoint{nova::crypto::Hash256{}, 0xFFFF'FFFFU}, {0x01U, 0x00U}, 0U}},
                       {{value, P2pkhScript(address)}},
                       0U};
}

std::optional<ExplorerBlock> MakeBlock(const std::uint32_t height,
                                       const nova::crypto::Hash256& previous,
                                       std::vector<Transaction> transactions)
{
    Block block{{1, previous, {}, 1'000U + height, 0x207F'FFFFU, height}, std::move(transactions)};
    const auto root = nova::primitives::ComputeMerkleRoot(
        block.transactions,
        nova::consensus::RegtestNetworkParams().block_limits.transaction_limits);
    if (!root.has_value()) {
        return std::nullopt;
    }
    block.header.merkle_root = *root;
    const auto hash = nova::primitives::ComputeBlockHash(block.header);
    if (!hash.has_value()) {
        return std::nullopt;
    }
    return ExplorerBlock{height, *hash, std::move(block)};
}

std::optional<ExplorerSnapshot> ValidSnapshot()
{
    const auto& params = nova::consensus::RegtestNetworkParams();
    const auto genesis = MakeBlock(0U, nova::crypto::Hash256{}, {Coinbase(100U, 0x11U)});
    if (!genesis.has_value()) {
        return std::nullopt;
    }
    const auto genesis_txid =
        genesis->block.transactions.front().TxId(params.block_limits.transaction_limits);
    if (!genesis_txid.has_value()) {
        return std::nullopt;
    }
    Transaction spend{
        1, {{OutPoint{*genesis_txid, 0U}, {0x51U}, 0U}}, {{90U, P2pkhScript(0x22U)}}, 0U};
    const auto next = MakeBlock(1U, genesis->hash, {Coinbase(50U, 0x11U), spend});
    if (!next.has_value()) {
        return std::nullopt;
    }
    return ExplorerSnapshot{params.id,
                            params.difficulty.target_spacing_seconds,
                            params.pow,
                            params.block_limits,
                            {*genesis, *next}};
}

std::string SocketRequest(nova::explorer::ExplorerLoopbackHttpServer& server,
                          const std::string_view request)
{
#ifdef _WIN32
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        ADD_FAILURE() << "unable to create explorer test socket";
        return {};
    }
#else
    const int socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket < 0) {
        ADD_FAILURE() << "unable to create explorer test socket";
        return {};
    }
#endif
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(server.port());
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0 ||
        send(socket, request.data(), static_cast<int>(request.size()), 0) !=
            static_cast<int>(request.size())) {
#ifdef _WIN32
        static_cast<void>(closesocket(socket));
#else
        static_cast<void>(close(socket));
#endif
        ADD_FAILURE() << "unable to submit explorer socket request";
        return {};
    }
    static_cast<void>(server.Pump(1U));
    static_cast<void>(server.Pump(1U));
    std::array<char, 4'096U> response{};
    const auto received = recv(socket, response.data(), static_cast<int>(response.size()), 0);
#ifdef _WIN32
    static_cast<void>(closesocket(socket));
#else
    static_cast<void>(close(socket));
#endif
    EXPECT_GT(received, 0);
    return received > 0 ? std::string{response.data(), static_cast<std::size_t>(received)}
                        : std::string{};
}

TEST(ExplorerIndexTests, IndexesReadOnlySnapshotAndSupportsRequiredSearches)
{
    const auto snapshot = ValidSnapshot();
    ASSERT_TRUE(snapshot.has_value());
    ExplorerIndex index;
    ASSERT_EQ(index.Rebuild(*snapshot).error, ExplorerError::kNone);

    const auto summary = index.Summary();
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->height, 1U);
    EXPECT_EQ(summary->indexed_blocks, 2U);
    EXPECT_EQ(summary->indexed_transactions, 3U);
    EXPECT_EQ(summary->unspent_outputs, 2U);
    EXPECT_GT(summary->estimated_blocks_per_day, 0U);
    EXPECT_EQ(index.AllUtxos(10U).size(), 2U);
    EXPECT_TRUE(index.AllUtxos(0U).empty());

    const auto latest = index.LatestBlocks(1U);
    ASSERT_EQ(latest.size(), 1U);
    EXPECT_EQ(latest.front().height, 1U);
    EXPECT_TRUE(index.FindBlockByHeight(0U).has_value());
    const auto found_block = index.FindBlockByHash(snapshot->active_blocks.at(1).hash);
    ASSERT_TRUE(found_block.has_value());
    EXPECT_EQ(found_block->height, 1U);

    const auto spend_id = snapshot->active_blocks.at(1).block.transactions.at(1).TxId(
        snapshot->block_limits.transaction_limits);
    ASSERT_TRUE(spend_id.has_value());
    const auto transaction = index.FindTransaction(*spend_id);
    ASSERT_TRUE(transaction.has_value());
    ASSERT_TRUE(transaction->fee.has_value());
    EXPECT_EQ(*transaction->fee, 10);

    Hash160 address{};
    address.fill(0x22U);
    const auto utxos = index.FindUtxosByAddress(address, 10U);
    ASSERT_EQ(utxos.size(), 1U);
    EXPECT_EQ(utxos.front().coin.output.value, 90);
}

TEST(ExplorerIndexTests, RejectsMissingInputAndPreservesPreviousIndex)
{
    const auto snapshot = ValidSnapshot();
    ASSERT_TRUE(snapshot.has_value());
    ExplorerIndex index;
    ASSERT_EQ(index.Rebuild(*snapshot).error, ExplorerError::kNone);
    const auto original = index.Summary();
    ASSERT_TRUE(original.has_value());

    auto invalid = *snapshot;
    invalid.active_blocks.at(1).block.transactions.at(1).inputs.at(0).previous_output.output_index =
        1U;
    const auto root = nova::primitives::ComputeMerkleRoot(
        invalid.active_blocks.at(1).block.transactions, invalid.block_limits.transaction_limits);
    ASSERT_TRUE(root.has_value());
    invalid.active_blocks.at(1).block.header.merkle_root = *root;
    const auto hash = nova::primitives::ComputeBlockHash(invalid.active_blocks.at(1).block.header);
    ASSERT_TRUE(hash.has_value());
    invalid.active_blocks.at(1).hash = *hash;

    EXPECT_EQ(index.Rebuild(invalid).error, ExplorerError::kMissingInput);
    const auto after = index.Summary();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->best_block, original->best_block);
    EXPECT_EQ(after->unspent_outputs, original->unspent_outputs);
}

TEST(ExplorerIndexTests, RecognizesOnlyExactP2pkhAddresses)
{
    const auto script = P2pkhScript(0x4AU);
    const auto address = nova::explorer::ExtractP2pkhAddress(script);
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(address->at(0), 0x4AU);
    auto malformed = script;
    malformed.at(2) = 0x13U;
    EXPECT_FALSE(nova::explorer::ExtractP2pkhAddress(malformed).has_value());
}

TEST(ExplorerIndexTests, ReadsCopiedSnapshotFromNovacoindWithoutMutation)
{
    const auto root = std::filesystem::temp_directory_path() / "novacoin-explorer-source";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_FALSE(error);

    const auto& parameters = nova::consensus::RegtestNetworkParams();
    auto node = nova::node::RegtestNode::Create({"explorer",
                                                 root / "data",
                                                 root / "logs" / "novacoind.log",
                                                 18'500U,
                                                 18'501U,
                                                 parameters.genesis_block.header.time,
                                                 {},
                                                 nova::consensus::NetworkId::kRegtest});
    ASSERT_NE(node, nullptr);
    const auto initial_height = node->height();

    nova::explorer::NovacoindSnapshotSource source{node->chain_state(), parameters};
    const auto snapshot = source.ReadSnapshot();
    ASSERT_TRUE(snapshot.has_value());
    ExplorerIndex index;
    ASSERT_EQ(index.Rebuild(*snapshot).error, ExplorerError::kNone);
    EXPECT_EQ(node->height(), initial_height);
    ASSERT_TRUE(index.Summary().has_value());
    EXPECT_EQ(index.Summary()->height, initial_height);

    node.reset();
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(ExplorerHttpTests, RequiresAuthenticationAndOnlyServesReadOnlyGetRoutes)
{
    const auto snapshot = ValidSnapshot();
    ASSERT_TRUE(snapshot.has_value());
    ExplorerIndex index;
    ASSERT_EQ(index.Rebuild(*snapshot).error, ExplorerError::kNone);
    const auto service = nova::explorer::ExplorerHttpService::Create(
        {"127.0.0.1", "viewer", "secret", {256U, 256U, 10U}}, index);
    ASSERT_NE(service, nullptr);
    EXPECT_EQ(nova::explorer::ExplorerHttpService::Create(
                  {"0.0.0.0", "viewer", "secret", {256U, 256U, 10U}}, index),
              nullptr);

    const nova::explorer::ExplorerHttpRequest authorized{"GET", "/api/v1/summary",
                                                         "Basic dmlld2VyOnNlY3JldA==", ""};
    const auto summary = service->Handle(authorized);
    EXPECT_EQ(summary.status, 200U);
    EXPECT_NE(summary.body.find("\"height\":1"), std::string::npos);

    const auto unauthenticated =
        service->Handle({"GET", "/api/v1/summary", "Basic Zm9vOmJhcg==", ""});
    EXPECT_EQ(unauthenticated.status, 401U);
    const auto noncanonical_authentication =
        service->Handle({"GET", "/api/v1/summary", "Basic dmlld2VyOnNlY3JldB==", ""});
    EXPECT_EQ(noncanonical_authentication.status, 401U);
    const auto post =
        service->Handle({"POST", "/api/v1/summary", "Basic dmlld2VyOnNlY3JldA==", ""});
    EXPECT_EQ(post.status, 405U);

    const auto latest =
        service->Handle({"GET", "/api/v1/blocks/latest?limit=1", "Basic dmlld2VyOnNlY3JldA==", ""});
    EXPECT_EQ(latest.status, 200U);
    EXPECT_NE(latest.body.find("\"transactions\""), std::string::npos);
    const auto utxos =
        service->Handle({"GET", "/api/v1/utxos?limit=2", "Basic dmlld2VyOnNlY3JldA==", ""});
    EXPECT_EQ(utxos.status, 200U);
    EXPECT_EQ(index.Summary()->height, 1U);
}

TEST(ExplorerHttpSocketTests, ServesOnlyBoundedAuthenticatedLoopbackGetRequests)
{
    const auto snapshot = ValidSnapshot();
    ASSERT_TRUE(snapshot.has_value());
    ExplorerIndex index;
    ASSERT_EQ(index.Rebuild(*snapshot).error, ExplorerError::kNone);
    const auto service = nova::explorer::ExplorerHttpService::Create(
        {"127.0.0.1", "viewer", "secret", {256U, 256U, 10U}}, index);
    ASSERT_NE(service, nullptr);
    EXPECT_EQ(nova::explorer::ExplorerLoopbackHttpServer::Create(
                  {"0.0.0.0", 0U, 4U, 256U, 256U, 4'096U, 30U}, *service),
              nullptr);
    const auto server = nova::explorer::ExplorerLoopbackHttpServer::Create(
        {"127.0.0.1", 0U, 4U, 256U, 256U, 4'096U, 30U}, *service);
    ASSERT_NE(server, nullptr);

    const auto valid = SocketRequest(
        *server, "GET /api/v1/summary HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Basic "
                 "dmlld2VyOnNlY3JldA==\r\n\r\n");
    EXPECT_TRUE(valid.starts_with("HTTP/1.1 200"));
    EXPECT_NE(valid.find("\"height\":1"), std::string::npos);
    const auto malformed = SocketRequest(
        *server, "GET /api/v1/summary HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(malformed.starts_with("HTTP/1.1 400"));
}

} // namespace
