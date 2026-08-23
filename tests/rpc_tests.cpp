#include <gtest/gtest.h>

#include "explorer/explorer.hpp"
#include "rpc/rpc.hpp"

#include "consensus/monetary.hpp"
#include "consensus/pow.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
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

#ifdef _WIN32
using TestSocketLength = int;
using TestSocketIoSize = int;
#else
using TestSocketLength = socklen_t;
using TestSocketIoSize = std::size_t;
#endif

using nova::chain::ChainAnchor;
using nova::chain::ChainState;
using nova::chain::ChainStateParams;
using nova::chain::ChainWork;
using nova::chain::Mempool;
using nova::chain::MempoolParams;
using nova::chain::UTXOSet;
using nova::consensus::ChainParams;
using nova::consensus::COIN;
using nova::consensus::INITIAL_SUBSIDY;
using nova::consensus::MAX_MONEY;
using nova::primitives::BlockLimits;
using nova::primitives::TransactionLimits;

constexpr TransactionLimits kTransactionLimits{MAX_MONEY, 100'000U, 8U, 8U, 128U};
constexpr BlockLimits kBlockLimits{kTransactionLimits, 100'000U, 8U, 64U};

nova::chain::BlockValidationParams RegtestValidationParams()
{
    return {kBlockLimits,
            nova::consensus::RegtestPowParameters(),
            ChainParams{COIN, MAX_MONEY, INITIAL_SUBSIDY, 10U},
            0U,
            500'000'000U,
            0xFFFF'FFFFU,
            1U,
            600U};
}

nova::crypto::Hash256 AnchorHash()
{
    nova::crypto::Hash256::Bytes bytes{};
    bytes.back() = 1U;
    return nova::crypto::Hash256{bytes};
}

std::unique_ptr<ChainState> CreateChainState(UTXOSet& utxos)
{
    const auto target = nova::consensus::TargetFromCompact(0x207F'FFFFU);
    if (!target.target.has_value()) {
        return nullptr;
    }
    const auto work = nova::consensus::CalculateWork(*target.target);
    if (!work.has_value()) {
        return nullptr;
    }
    return ChainState::Create(
        ChainStateParams{RegtestValidationParams(),
                         nova::consensus::RegtestNetworkParams().difficulty, 3U,
                         std::make_shared<nova::chain::FixedValidationTimeSource>(10'000U)},
        ChainAnchor{AnchorHash(), nova::crypto::Hash256{}, 0U, 1'000U, *target.target, *work,
                    ChainWork::FromPerBlockWork(*work)},
        utxos);
}

nova::net::PeerManager MakePeerManager()
{
    const nova::net::P2PParams protocol{0xDAB5'BFFAU, 1,  100'000U,           64U,         8U, 8U,
                                        8U,           8U, kTransactionLimits, kBlockLimits};
    const nova::net::ConnectionParams connection{
        protocol, {1, 0U, 1'000, 1U, "/rpc-test/", 0, true}, 10U, 20U, 10U, 8U, 8U, 100'000U};
    return nova::net::PeerManager{nova::net::PeerManagerParams{connection, 2U, 2U}};
}

std::unique_ptr<nova::rpc::RpcService> CreateService(const std::string_view bind = "127.0.0.1",
                                                     const bool regtest = true,
                                                     const bool harness_callbacks = false)
{
    static UTXOSet utxos;
    static auto state = CreateChainState(utxos);
    static auto mempool =
        Mempool::Create(MempoolParams{RegtestValidationParams(), 16U, 100'000U, 0});
    static auto peers = MakePeerManager();
    static auto wallet =
        nova::wallet::Wallet::Create(nova::wallet::WalletParams{kTransactionLimits, 1, 1U});
    if (state == nullptr || mempool == nullptr || wallet == nullptr) {
        return nullptr;
    }
    nova::rpc::RpcDependencies dependencies{
        *state, utxos, *mempool, peers, *wallet, {1U, 1'000U}, 1'000U, {}, {}, {}, {}, {}, {}};
    if (harness_callbacks) {
        dependencies.mine_regtest_block = []() -> std::optional<nova::crypto::Hash256> {
            nova::crypto::Hash256::Bytes bytes{};
            bytes.back() = 0xA1U;
            return nova::crypto::Hash256{bytes};
        };
        dependencies.send_to_address =
            [](const nova::wallet::Recipient&) -> std::optional<nova::crypto::Hash256> {
            nova::crypto::Hash256::Bytes bytes{};
            bytes.back() = 0xB2U;
            return nova::crypto::Hash256{bytes};
        };
        dependencies.connect_peer = [](const nova::net::TcpEndpoint&) { return true; };
        dependencies.disconnect_peers = []() {};
    }
    return nova::rpc::RpcService::Create(
        nova::rpc::RpcConfig{std::string{bind},
                             "user",
                             "pass",
                             {256U, 8'192U, 4'096U},
                             regtest ? nova::consensus::NetworkId::kRegtest
                                     : nova::consensus::NetworkId::kTestnet},
        std::move(dependencies));
}

std::string SocketRequest(nova::rpc::LoopbackHttpServer& server, const std::string_view request)
{
#ifdef _WIN32
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        ADD_FAILURE() << "unable to create socket";
        return {};
    }
#else
    const int socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket < 0) {
        ADD_FAILURE() << "unable to create socket";
        return {};
    }
#endif
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(server.port());
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto connected = connect(socket, reinterpret_cast<const sockaddr*>(&endpoint),
                                   static_cast<TestSocketLength>(sizeof(endpoint)));
    const auto sent = connected == 0
                          ? send(socket, request.data(),
                                 static_cast<TestSocketIoSize>(request.size()), 0)
                          : -1;
    if (connected != 0 ||
        sent < 0 || static_cast<std::size_t>(sent) != request.size()) {
#ifdef _WIN32
        static_cast<void>(closesocket(socket));
#else
        static_cast<void>(close(socket));
#endif
        ADD_FAILURE() << "unable to submit socket request";
        return {};
    }
    static_cast<void>(server.Pump(1U));
    static_cast<void>(server.Pump(1U));
    std::array<char, 4'096U> response{};
    const auto received =
        recv(socket, response.data(), static_cast<TestSocketIoSize>(response.size()), 0);
#ifdef _WIN32
    static_cast<void>(closesocket(socket));
#else
    static_cast<void>(close(socket));
#endif
    EXPECT_GT(received, 0);
    return received > 0 ? std::string{response.data(), static_cast<std::size_t>(received)}
                        : std::string{};
}

TEST(RpcIntegration, RequiresLoopbackBindingAndBasicAuthentication)
{
    EXPECT_EQ(CreateService("0.0.0.0"), nullptr);
    auto service = CreateService();
    ASSERT_NE(service, nullptr);
    EXPECT_EQ(service->bind_address(), "127.0.0.1");

    const auto rejected = service->HandleHttpPost(
        {"POST", "Basic dXNlcjp3cm9uZw==", R"({"jsonrpc":"2.0","id":1,"method":"getbalances"})"});
    EXPECT_EQ(rejected.status, 401U);
    EXPECT_NE(rejected.body.find("-32001"), std::string::npos);
}

TEST(RpcIntegration, ServesReadWalletAndStructuredFailureResponses)
{
    auto service = CreateService();
    ASSERT_NE(service, nullptr);
    constexpr std::string_view kAuth{"Basic dXNlcjpwYXNz"};

    const auto info = service->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":1,"method":"getblockchaininfo"})"});
    EXPECT_EQ(info.status, 200U);
    EXPECT_NE(info.body.find("\"blocks\":0"), std::string::npos);

    const auto address = service->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":"a","method":"getnewaddress"})"});
    EXPECT_EQ(address.status, 200U);
    EXPECT_NE(address.body.find("\"result\":\""), std::string::npos);
    EXPECT_EQ(address.body.find("private"), std::string::npos);

    const auto unknown =
        service->HandleHttpPost({"POST", kAuth, R"({"jsonrpc":"2.0","id":2,"method":"unknown"})"});
    EXPECT_EQ(unknown.status, 200U);
    EXPECT_NE(unknown.body.find("-32601"), std::string::npos);

    const auto malformed = service->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":3,"method":"getbalances")"});
    EXPECT_EQ(malformed.status, 400U);
    EXPECT_NE(malformed.body.find("-32600"), std::string::npos);
}

TEST(RpcIntegration, ServesAuthenticatedReadOnlyHealthAndMetrics)
{
    auto service = CreateService();
    ASSERT_NE(service, nullptr);
    constexpr std::string_view kAuth{"Basic dXNlcjpwYXNz"};

    const auto health = service->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":7,"method":"getnodehealth"})"});
    EXPECT_EQ(health.status, 200U);
    EXPECT_NE(health.body.find("\"ready\":true"), std::string::npos);
    EXPECT_NE(health.body.find("\"network\":\"regtest\""), std::string::npos);
    EXPECT_NE(health.body.find("\"blocks_accepted\":0"), std::string::npos);

    const auto metrics = service->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":8,"method":"getnodemetrics"})"});
    EXPECT_EQ(metrics.status, 200U);
    EXPECT_NE(metrics.body.find("\"p2p_transport_errors\":0"), std::string::npos);

    const auto info = service->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":9,"method":"getblockchaininfo"})"});
    EXPECT_NE(info.body.find("\"blocks\":0"), std::string::npos);
}

TEST(RpcIntegration, ValidatesWalletAndRegtestArguments)
{
    auto service = CreateService();
    ASSERT_NE(service, nullptr);
    constexpr std::string_view kAuth{"Basic dXNlcjpwYXNz"};
    const auto invalid_create = service->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":4,"method":"createtransaction","params":{}})"});
    EXPECT_NE(invalid_create.body.find("-32602"), std::string::npos);

    const auto invalid_mine = service->HandleHttpPost(
        {"POST", kAuth,
         R"({"jsonrpc":"2.0","id":5,"method":"mineregtestheader","params":{"header":"00","max_attempts":1}})"});
    EXPECT_NE(invalid_mine.body.find("-32602"), std::string::npos);

    auto disabled = CreateService("127.0.0.1", false);
    ASSERT_NE(disabled, nullptr);
    const auto refused = disabled->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":6,"method":"mineregtestheader"})"});
    EXPECT_NE(refused.body.find("-32005"), std::string::npos);
}

TEST(RpcIntegration, RestrictsAndDelegatesRegtestHarnessOperations)
{
    constexpr std::string_view kAuth{"Basic dXNlcjpwYXNz"};
    auto unavailable = CreateService();
    ASSERT_NE(unavailable, nullptr);
    const auto missing_callback = unavailable->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":1,"method":"generateregtestblock"})"});
    EXPECT_NE(missing_callback.body.find("-32005"), std::string::npos);

    auto service = CreateService("127.0.0.1", true, true);
    ASSERT_NE(service, nullptr);
    const auto mined = service->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":2,"method":"generateregtestblock"})"});
    EXPECT_EQ(mined.status, 200U);
    EXPECT_NE(mined.body.find("a1"), std::string::npos);
    const auto sent = service->HandleHttpPost(
        {"POST", kAuth,
         R"({"jsonrpc":"2.0","id":3,"method":"sendtoaddress","params":{"address":"0000000000000000000000000000000000000000","amount":1}})"});
    EXPECT_EQ(sent.status, 200U);
    EXPECT_NE(sent.body.find("b2"), std::string::npos);
    const auto connected = service->HandleHttpPost(
        {"POST", kAuth,
         R"({"jsonrpc":"2.0","id":4,"method":"connectpeer","params":{"host":"127.0.0.1","port":18444}})"});
    EXPECT_NE(connected.body.find("\"result\":true"), std::string::npos);
    const auto rejected_host = service->HandleHttpPost(
        {"POST", kAuth,
         R"({"jsonrpc":"2.0","id":5,"method":"connectpeer","params":{"host":"198.51.100.1","port":18444}})"});
    EXPECT_NE(rejected_host.body.find("-32004"), std::string::npos);
}

TEST(RpcSocketIntegration, ServesAuthenticatedBoundedLoopbackRequests)
{
    auto service = CreateService();
    ASSERT_NE(service, nullptr);
    auto server =
        nova::rpc::LoopbackHttpServer::Create({"127.0.0.1", 0U, 4U, 256U, 8'192U, 30U}, *service);
    ASSERT_NE(server, nullptr);
    ASSERT_NE(server->port(), 0U);

    constexpr std::string_view kBody{
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockchaininfo\"}"};
    const std::string valid = "POST /rpc HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Basic "
                              "dXNlcjpwYXNz\r\nContent-Length: " +
                              std::to_string(kBody.size()) + "\r\n\r\n" + std::string{kBody};
    const auto accepted = SocketRequest(*server, valid);
    EXPECT_TRUE(accepted.starts_with("HTTP/1.1 200"));
    EXPECT_NE(accepted.find("\"blocks\":0"), std::string::npos);

    constexpr std::string_view kHealthBody{
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"getnodehealth\"}"};
    const std::string health = "POST /rpc HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Basic "
                               "dXNlcjpwYXNz\r\nContent-Length: " +
                               std::to_string(kHealthBody.size()) + "\r\n\r\n" +
                               std::string{kHealthBody};
    const auto health_response = SocketRequest(*server, health);
    EXPECT_TRUE(health_response.starts_with("HTTP/1.1 200"));
    EXPECT_NE(health_response.find("\"ready\":true"), std::string::npos);

    const std::string unauthenticated =
        "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " + std::to_string(kBody.size()) +
        "\r\n\r\n" + std::string{kBody};
    const auto rejected = SocketRequest(*server, unauthenticated);
    EXPECT_TRUE(rejected.starts_with("HTTP/1.1 401"));
    EXPECT_NE(rejected.find("-32001"), std::string::npos);
}

TEST(RpcSocketIntegration, RejectsMalformedAndNonLoopbackServerConfigurations)
{
    auto service = CreateService();
    ASSERT_NE(service, nullptr);
    EXPECT_EQ(
        nova::rpc::LoopbackHttpServer::Create({"0.0.0.0", 0U, 4U, 256U, 1'024U, 30U}, *service),
        nullptr);
    auto server =
        nova::rpc::LoopbackHttpServer::Create({"127.0.0.1", 0U, 4U, 256U, 8'192U, 30U}, *service);
    ASSERT_NE(server, nullptr);
    const auto malformed = SocketRequest(
        *server, "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    EXPECT_TRUE(malformed.starts_with("HTTP/1.1 400"));
}

TEST(ExplorerRpcSnapshotIntegration, AcceptsOnlyAuthenticatedCopiedSnapshots)
{
    auto service = CreateService();
    ASSERT_NE(service, nullptr);
    auto server =
        nova::rpc::LoopbackHttpServer::Create({"127.0.0.1", 0U, 4U, 256U, 8'192U, 30U}, *service);
    ASSERT_NE(server, nullptr);
    const auto source = nova::explorer::AuthenticatedRpcSnapshotSource::Create(
        {"127.0.0.1", server->port(), "user", "pass", 1U * 1024U * 1024U,
         &nova::consensus::RegtestNetworkParams()});
    ASSERT_NE(source, nullptr);
    const auto wrong_credentials = nova::explorer::AuthenticatedRpcSnapshotSource::Create(
        {"127.0.0.1", server->port(), "user", "wrong", 1U * 1024U * 1024U,
         &nova::consensus::RegtestNetworkParams()});
    ASSERT_NE(wrong_credentials, nullptr);

    std::atomic<bool> running{true};
    std::thread pump{[&server, &running]() {
        while (running.load()) {
            static_cast<void>(server->Pump(1U));
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }};
    const auto snapshot = source->ReadSnapshot();
    const auto rejected = wrong_credentials->ReadSnapshot();
    running.store(false);
    pump.join();

    ASSERT_TRUE(snapshot.has_value());
    EXPECT_FALSE(rejected.has_value());
    nova::explorer::ExplorerIndex index;
    EXPECT_EQ(index.Rebuild(*snapshot).error, nova::explorer::ExplorerError::kNone);
    ASSERT_TRUE(index.Summary().has_value());
    EXPECT_EQ(index.Summary()->height, 0U);
}

} // namespace
