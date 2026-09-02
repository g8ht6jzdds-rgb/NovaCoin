#include <gtest/gtest.h>

#include "faucet/faucet.hpp"
#include "faucet/http.hpp"
#include "faucet/http_server.hpp"
#include "wallet/address.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

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

std::string SocketRequest(nova::faucet::FaucetLoopbackHttpServer& server,
                          const std::string_view request)
{
#ifdef _WIN32
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        ADD_FAILURE() << "unable to create faucet test socket";
        return {};
    }
#else
    const int socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket < 0) {
        ADD_FAILURE() << "unable to create faucet test socket";
        return {};
    }
#endif
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(server.port());
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto connected = connect(socket, reinterpret_cast<const sockaddr*>(&endpoint),
                                   static_cast<TestSocketLength>(sizeof(endpoint)));
    const auto sent = connected == 0 ? send(socket, request.data(),
                                            static_cast<TestSocketIoSize>(request.size()), 0)
                                     : -1;
    if (connected != 0 || sent < 0 || static_cast<std::size_t>(sent) != request.size()) {
#ifdef _WIN32
        static_cast<void>(closesocket(socket));
#else
        static_cast<void>(close(socket));
#endif
        ADD_FAILURE() << "unable to submit faucet socket request";
        return {};
    }
    static_cast<void>(server.Pump(100U));
    static_cast<void>(server.Pump(100U));
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

class FaucetServiceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        audit_ = std::filesystem::temp_directory_path() / "novacoin-faucet-test" / "audit.log";
        std::error_code error;
        std::filesystem::remove_all(audit_.parent_path(), error);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(audit_.parent_path(), error);
    }

    [[nodiscard]] std::unique_ptr<nova::faucet::FaucetService> Create()
    {
        return nova::faucet::FaucetService::Create(
            nova::consensus::TestnetNetworkParams(), {10U, 10U, 60U, 1U, 2U, 64U}, audit_,
            [](const nova::wallet::Recipient&) -> std::optional<nova::crypto::Hash256> {
                nova::crypto::Hash256::Bytes transaction{};
                transaction.back() = 0x42U;
                return nova::crypto::Hash256{transaction};
            });
    }

    std::filesystem::path audit_;
};

TEST_F(FaucetServiceTest, PaysOnlyTestnetAddressAndWritesDurableAuditWithoutSourceIdentity)
{
    auto service = Create();
    ASSERT_NE(service, nullptr);
    nova::crypto::Hash160 key_hash{};
    const auto address =
        nova::wallet::EncodeP2pkhAddress(key_hash, nova::consensus::TestnetNetworkParams());
    ASSERT_TRUE(address.has_value());
    const auto paid = service->RequestPayout({"requester-opaque-token", *address, 10U, 100U});
    ASSERT_EQ(paid.error, nova::faucet::FaucetError::kNone);
    ASSERT_TRUE(paid.transaction_id.has_value());
    EXPECT_TRUE(std::filesystem::is_regular_file(audit_));
    std::ifstream audit{audit_};
    const std::string audit_text{std::istreambuf_iterator<char>{audit},
                                 std::istreambuf_iterator<char>{}};
    EXPECT_NE(audit_text.find("source_sha256="), std::string::npos);
    EXPECT_EQ(audit_text.find("requester-opaque-token"), std::string::npos);
    const auto wrong_network =
        nova::wallet::EncodeP2pkhAddress(key_hash, nova::consensus::RegtestNetworkParams());
    ASSERT_TRUE(wrong_network.has_value());
    EXPECT_EQ(service->RequestPayout({"other", *wrong_network, 10U, 100U}).error,
              nova::faucet::FaucetError::kWrongNetworkAddress);
}

TEST_F(FaucetServiceTest, EnforcesRateLimitMaximumPayoutAndKillSwitch)
{
    auto service = Create();
    ASSERT_NE(service, nullptr);
    nova::crypto::Hash160 key_hash{};
    const auto address =
        nova::wallet::EncodeP2pkhAddress(key_hash, nova::consensus::TestnetNetworkParams());
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(service->RequestPayout({"same", *address, 11U, 100U}).error,
              nova::faucet::FaucetError::kInvalidRequest);
    EXPECT_EQ(service->RequestPayout({"same", *address, 10U, 100U}).error,
              nova::faucet::FaucetError::kNone);
    EXPECT_EQ(service->RequestPayout({"same", *address, 10U, 101U}).error,
              nova::faucet::FaucetError::kRateLimited);
    service->SetEnabled(false);
    EXPECT_EQ(service->RequestPayout({"new", *address, 10U, 200U}).error,
              nova::faucet::FaucetError::kDisabled);
    EXPECT_FALSE(service->metrics().enabled);
}

TEST(FaucetService, RefusesAnyNetworkOtherThanTestnet)
{
    EXPECT_EQ(nova::faucet::FaucetService::Create(
                  nova::consensus::RegtestNetworkParams(), {1U, 1U, 1U, 1U, 1U, 1U}, "audit.log",
                  [](const nova::wallet::Recipient&) { return std::nullopt; }),
              nullptr);
}

TEST_F(FaucetServiceTest, HttpTransportRejectsMalformedAndWrongNetworkRequestsBeforePayout)
{
    auto service = Create();
    ASSERT_NE(service, nullptr);
    const auto transport =
        nova::faucet::FaucetHttpService::Create({"127.0.0.1", {256U, 64U}}, *service);
    ASSERT_NE(transport, nullptr);
    const auto wrong_method = transport->Handle({"GET", "/api/v1/request", "", "127.0.0.1", 100U});
    EXPECT_EQ(wrong_method.status, 405U);
    const auto malformed = transport->Handle(
        {"POST", "/api/v1/request", "{\"amount\":10,\"address\":\"x\"}", "127.0.0.1", 100U});
    EXPECT_EQ(malformed.status, 400U);
    nova::crypto::Hash160 key_hash{};
    const auto regtest =
        nova::wallet::EncodeP2pkhAddress(key_hash, nova::consensus::RegtestNetworkParams());
    ASSERT_TRUE(regtest.has_value());
    const auto wrong_network =
        transport->Handle({"POST", "/api/v1/request",
                           "{\"address\":\"" + *regtest + "\",\"amount\":10}", "127.0.0.1", 100U});
    EXPECT_EQ(wrong_network.status, 400U);
    EXPECT_NE(wrong_network.body.find("wrong_network_address"), std::string::npos)
        << wrong_network.body;
}

TEST_F(FaucetServiceTest, SocketTransportIsLoopbackOnlyBoundedAndUsesPeerIdentity)
{
    auto service = Create();
    ASSERT_NE(service, nullptr);
    const auto transport =
        nova::faucet::FaucetHttpService::Create({"127.0.0.1", {256U, 64U}}, *service);
    ASSERT_NE(transport, nullptr);
    EXPECT_EQ(nova::faucet::FaucetLoopbackHttpServer::Create(
                  {"0.0.0.0", 0U, 2U, 256U, 256U, 1'024U, 30U}, *transport),
              nullptr);
    EXPECT_EQ(nova::faucet::FaucetLoopbackHttpServer::Create(
                  {"127.0.0.1", 0U, 2U, (std::numeric_limits<std::size_t>::max)(), 1U, 1'024U, 30U},
                  *transport),
              nullptr);
    const auto server = nova::faucet::FaucetLoopbackHttpServer::Create(
        {"127.0.0.1", 0U, 2U, 256U, 256U, 1'024U, 30U}, *transport);
    ASSERT_NE(server, nullptr);
    nova::crypto::Hash160 key_hash{};
    const auto address =
        nova::wallet::EncodeP2pkhAddress(key_hash, nova::consensus::TestnetNetworkParams());
    ASSERT_TRUE(address.has_value());
    const std::string body{"{\"address\":\"" + *address + "\",\"amount\":10}"};
    const std::string request{
        "POST /api/v1/request HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body};
    const auto paid = SocketRequest(*server, request);
    EXPECT_TRUE(paid.starts_with("HTTP/1.1 200")) << paid;
    EXPECT_NE(paid.find("\"txid\""), std::string::npos);
    const auto repeat = SocketRequest(*server, request);
    EXPECT_TRUE(repeat.starts_with("HTTP/1.1 429")) << repeat;
    const auto malformed = SocketRequest(
        *server, "POST /api/v1/request HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(malformed.starts_with("HTTP/1.1 400"));
}

} // namespace
