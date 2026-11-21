#include <gtest/gtest.h>

#include "build/version.hpp"
#include "explorer/explorer.hpp"
#include "rpc/rpc.hpp"
#include "wallet/address.hpp"

#include "chain/network_chainstate.hpp"
#include "consensus/monetary.hpp"
#include "consensus/pow.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

std::unique_ptr<nova::rpc::RpcService>
CreateService(const std::string_view bind = "127.0.0.1", const bool regtest = true,
              const bool harness_callbacks = false, const bool restricted_credentials = false,
              const std::optional<nova::consensus::NetworkId> network_override = std::nullopt)
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
    nova::rpc::RpcConfig config{
        std::string{bind},
        "user",
        "pass",
        {256U, 8'192U, 4'096U},
        network_override.value_or(regtest ? nova::consensus::NetworkId::kRegtest
                                          : nova::consensus::NetworkId::kTestnet)};
    if (restricted_credentials) {
        config.restricted_principals = {{"explorer", "view", nova::rpc::RpcRole::kExplorer},
                                        {"faucet", "faucet", nova::rpc::RpcRole::kFaucet}};
        config.maximum_faucet_payout = 10U;
    }
    return nova::rpc::RpcService::Create(std::move(config), std::move(dependencies));
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
    const auto sent = connected == 0 ? send(socket, request.data(),
                                            static_cast<TestSocketIoSize>(request.size()), 0)
                                     : -1;
    if (connected != 0 || sent < 0 || static_cast<std::size_t>(sent) != request.size()) {
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

TEST(RpcIntegration, RejectsUnknownAndLockedNetworksAtFactoryAdmission)
{
    for (const auto network :
         {nova::consensus::NetworkId::kMainnet, static_cast<nova::consensus::NetworkId>(3U),
          static_cast<nova::consensus::NetworkId>(255U)}) {
        SCOPED_TRACE(static_cast<unsigned>(network));
        for (const auto bind : {"127.0.0.1", "::1"}) {
            EXPECT_EQ(CreateService(bind, true, false, false, network), nullptr);
        }
    }
    for (const auto network :
         {nova::consensus::NetworkId::kRegtest, nova::consensus::NetworkId::kTestnet}) {
        for (const auto bind : {"127.0.0.1", "::1"}) {
            EXPECT_NE(CreateService(bind, true, false, false, network), nullptr);
        }
    }
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

TEST(RpcIntegration, RestrictsExplorerAndFaucetCredentialsToDedicatedMethods)
{
    auto service = CreateService("127.0.0.1", false, true, true);
    ASSERT_NE(service, nullptr);
    constexpr std::string_view kExplorer{"Basic ZXhwbG9yZXI6dmlldw=="};
    constexpr std::string_view kFaucet{"Basic ZmF1Y2V0OmZhdWNldA=="};
    const auto snapshot = service->HandleHttpPost(
        {"POST", kExplorer, R"({"jsonrpc":"2.0","id":1,"method":"getexplorersnapshot"})"});
    EXPECT_EQ(snapshot.status, 200U);
    EXPECT_NE(snapshot.body.find("nova-snapshot-v1"), std::string::npos);
    const auto explorer_denied = service->HandleHttpPost(
        {"POST", kExplorer, R"({"jsonrpc":"2.0","id":2,"method":"getnewaddress"})"});
    EXPECT_EQ(explorer_denied.status, 403U);
    EXPECT_NE(explorer_denied.body.find("-32006"), std::string::npos);

    nova::crypto::Hash160 recipient_hash{};
    const auto address =
        nova::wallet::EncodeP2pkhAddress(recipient_hash, nova::consensus::TestnetNetworkParams());
    ASSERT_TRUE(address.has_value());
    const auto paid = service->HandleHttpPost(
        {"POST", kFaucet,
         "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"faucetpay\",\"params\":{\"address\":\"" +
             *address + "\",\"amount\":10}}"});
    EXPECT_EQ(paid.status, 200U);
    EXPECT_NE(paid.body.find("b2"), std::string::npos);
    const auto faucet_denied = service->HandleHttpPost(
        {"POST", kFaucet, R"({"jsonrpc":"2.0","id":4,"method":"sendtoaddress"})"});
    EXPECT_EQ(faucet_denied.status, 403U);

    const auto oversized = service->HandleHttpPost(
        {"POST", kFaucet,
         "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"faucetpay\",\"params\":{\"address\":\"" +
             *address + "\",\"amount\":11}}"});
    EXPECT_NE(oversized.body.find("-32602"), std::string::npos);
}

TEST(RpcIntegration, ReportsCentralVersionAndActualNetworkCompatibility)
{
    for (const auto regtest : {true, false}) {
        auto service = CreateService("127.0.0.1", regtest, false, true);
        ASSERT_NE(service, nullptr);
        const auto before =
            service->HandleHttpPost({"POST", "Basic dXNlcjpwYXNz",
                                     R"({"jsonrpc":"2.0","id":1,"method":"getblockchaininfo"})"});
        const auto info =
            service->HandleHttpPost({"POST", "Basic dXNlcjpwYXNz",
                                     R"({"jsonrpc":"2.0","id":1,"method":"getnetworkinfo"})"});
        const auto& network = regtest ? nova::consensus::RegtestNetworkParams()
                                      : nova::consensus::TestnetNetworkParams();
        EXPECT_EQ(info.status, 200U);
        EXPECT_NE(
            info.body.find("\"version\":\"" + std::string{nova::build::kApplicationVersion} + "\""),
            std::string::npos);
        EXPECT_NE(info.body.find("\"protocolversion\":" + std::to_string(network.protocol_version)),
                  std::string::npos);
        EXPECT_NE(info.body.find("\"minimumpeerprotocolversion\":" +
                                 std::to_string(network.minimum_peer_protocol_version)),
                  std::string::npos);
        const auto after =
            service->HandleHttpPost({"POST", "Basic dXNlcjpwYXNz",
                                     R"({"jsonrpc":"2.0","id":1,"method":"getblockchaininfo"})"});
        EXPECT_EQ(before.body, after.body);
        EXPECT_EQ(service
                      ->HandleHttpPost({"POST", "Basic invalid",
                                        R"({"jsonrpc":"2.0","id":1,"method":"getnetworkinfo"})"})
                      .status,
                  401U);
        EXPECT_EQ(service
                      ->HandleHttpPost({"POST", "Basic ZXhwbG9yZXI6dmlldw==",
                                        R"({"jsonrpc":"2.0","id":1,"method":"getnetworkinfo"})"})
                      .status,
                  403U);
    }
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
    EXPECT_NE(health.body.find("\"bestblockhash\":"), std::string::npos);
    EXPECT_NE(health.body.find("\"chainwork\":"), std::string::npos);
    EXPECT_NE(health.body.find("\"mempool_bytes\":0"), std::string::npos);
    EXPECT_NE(health.body.find("\"uptime_seconds\":0"), std::string::npos);
    EXPECT_NE(health.body.find("\"last_block_arrival_time_seconds\":0"), std::string::npos);
    EXPECT_NE(health.body.find("\"validation_failures\":0"), std::string::npos);

    const auto metrics = service->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":8,"method":"getnodemetrics"})"});
    EXPECT_EQ(metrics.status, 200U);
    EXPECT_NE(metrics.body.find("\"p2p_transport_errors\":0"), std::string::npos);
    EXPECT_NE(metrics.body.find("\"validation_failures\":0"), std::string::npos);

    const auto info = service->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":9,"method":"getblockchaininfo"})"});
    EXPECT_NE(info.body.find("\"blocks\":0"), std::string::npos);
}

TEST(RpcIntegration, WalletSubmissionUsesNodeCallbackAndHonorsRejection)
{
    UTXOSet utxos;
    auto state = CreateChainState(utxos);
    auto mempool = Mempool::Create({RegtestValidationParams(), 16U, 100000U, 0});
    auto peers = MakePeerManager();
    auto wallet = nova::wallet::Wallet::Create({kTransactionLimits, 1, 1U});
    ASSERT_NE(state, nullptr);
    ASSERT_NE(mempool, nullptr);
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    std::vector<std::uint8_t> script{0x76U, 0xA9U, 0x14U};
    script.insert(script.end(), address->public_key_hash.begin(), address->public_key_hash.end());
    script.insert(script.end(), {0x88U, 0xACU});
    const nova::primitives::Transaction funding{
        1,
        {{nova::primitives::OutPoint{AnchorHash(), 0U}, {}, 0xFFFFFFFFU}},
        {{5LL * COIN, script}},
        0U};
    ASSERT_TRUE(wallet->ObserveTransaction(funding, 1U, true));
    const std::vector<nova::wallet::Recipient> recipients{{address->public_key_hash, COIN}};
    const auto built = wallet->CreateTransaction(recipients);
    ASSERT_TRUE(built.built.has_value());
    nova::primitives::BinaryWriter writer;
    ASSERT_TRUE(built.built->transaction.Serialize(writer, kTransactionLimits));
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (const auto byte : writer.bytes()) {
        hex.push_back(digits[byte >> 4U]);
        hex.push_back(digits[byte & 15U]);
    }
    bool accept = false;
    unsigned calls = 0;
    unsigned address_calls = 0;
    unsigned persist_calls = 0;
    bool address_success = false;
    bool state_available = true;
    bool lose_authority = false;
    bool wallet_available = true;
    bool lookup_available = true;
    std::optional<bool> verification{true};
    nova::rpc::RpcDependencies dependencies{
        *state, utxos, *mempool, peers, *wallet, {1U, 1000U}, 1000U, {}, {}, {}, {}, {}, {}};
    dependencies.submit_transaction = [&](const nova::primitives::Transaction& transaction) {
        ++calls;
        EXPECT_EQ(transaction.TxId(kTransactionLimits),
                  built.built->transaction.TxId(kTransactionLimits));
        if (lose_authority) {
            state_available = false;
            return false;
        }
        return accept;
    };
    dependencies.generate_address = [&]() -> std::optional<nova::wallet::WalletAddress> {
        ++address_calls;
        if (lose_authority) {
            state_available = false;
            return std::nullopt;
        }
        return address_success ? address : std::nullopt;
    };
    dependencies.mine_regtest_block = [&]() -> std::optional<nova::crypto::Hash256> {
        if (lose_authority)
            state_available = false;
        return std::nullopt;
    };
    dependencies.send_to_address =
        [&](const nova::wallet::Recipient&) -> std::optional<nova::crypto::Hash256> {
        if (lose_authority)
            state_available = false;
        return std::nullopt;
    };
    dependencies.persist_wallet = [&]() {
        ++persist_calls;
        return false;
    };
    dependencies.state_available = [&]() { return state_available; };
    dependencies.verify_transaction = [&](const nova::primitives::Transaction&) {
        return verification;
    };
    dependencies.read_wallet = [&]() -> std::optional<nova::rpc::RpcWalletSnapshot> {
        if (!wallet_available)
            return std::nullopt;
        return nova::rpc::RpcWalletSnapshot{wallet->ConfirmedBalance(),
                                            wallet->UnconfirmedBalance(), wallet->ListUTXOs()};
    };
    dependencies.build_transaction = [&](std::span<const nova::wallet::Recipient> requested)
        -> std::optional<nova::wallet::TransactionBuildResult> {
        return wallet_available ? std::optional{wallet->CreateTransaction(requested)}
                                : std::nullopt;
    };
    dependencies.sign_transaction = [&](const nova::primitives::Transaction& transaction)
        -> std::optional<nova::wallet::TransactionSignResult> {
        return wallet_available ? std::optional{wallet->SignTransaction(transaction)}
                                : std::nullopt;
    };
    dependencies.find_block = [&](const nova::crypto::Hash256& hash) {
        return nova::rpc::RpcBlockLookup{
            lookup_available, lookup_available ? state->GetBlockIndex(hash) : std::nullopt};
    };
    dependencies.find_mempool_entry = [&](const nova::crypto::Hash256& hash) {
        return nova::rpc::RpcMempoolLookup{
            lookup_available, lookup_available ? mempool->GetEntry(hash) : std::nullopt};
    };
    dependencies.read_coin = [&](const nova::chain::UTXOKey& key) {
        return lookup_available ? utxos.ReadCoin(key)
                                : nova::chain::CoinReadResult{
                                      nova::chain::CoinReadError::kUnavailable, std::nullopt};
    };
    dependencies.read_chain = [&]() -> std::optional<nova::rpc::RpcChainSnapshot> {
        if (!lookup_available)
            return std::nullopt;
        const auto index = state->GetBlockIndex(state->active_tip());
        if (!index)
            return std::nullopt;
        return nova::rpc::RpcChainSnapshot{state->active_height(), state->active_tip(),
                                           index->chain_work, mempool->size(),
                                           mempool->total_serialized_size()};
    };
    dependencies.read_active_blocks = [&]() -> std::optional<std::vector<nova::chain::BlockIndex>> {
        return lookup_available ? state->GetActiveBlockIndexes() : std::nullopt;
    };
    nova::rpc::RpcConfig required{
        "127.0.0.1", "user", "pass", {256U, 8192U, 4096U}, nova::consensus::NetworkId::kRegtest};
    required.require_node_mutation_callbacks = true;
    auto incomplete = dependencies;
    incomplete.generate_address = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(incomplete)), nullptr);
    auto missing_submission = dependencies;
    missing_submission.submit_transaction = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(missing_submission)), nullptr);
    auto missing_guard = dependencies;
    missing_guard.state_available = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(missing_guard)), nullptr);
    auto missing_verifier = dependencies;
    missing_verifier.verify_transaction = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(missing_verifier)), nullptr);
    auto missing_wallet = dependencies;
    missing_wallet.read_wallet = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(missing_wallet)), nullptr);
    auto missing_builder = dependencies;
    missing_builder.build_transaction = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(missing_builder)), nullptr);
    auto missing_signer = dependencies;
    missing_signer.sign_transaction = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(missing_signer)), nullptr);
    auto missing_block_reader = dependencies;
    missing_block_reader.find_block = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(missing_block_reader)), nullptr);
    auto missing_mempool_reader = dependencies;
    missing_mempool_reader.find_mempool_entry = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(missing_mempool_reader)), nullptr);
    auto missing_coin_reader = dependencies;
    missing_coin_reader.read_coin = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(missing_coin_reader)), nullptr);
    auto missing_summary = dependencies;
    missing_summary.read_chain = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(missing_summary)), nullptr);
    auto missing_active = dependencies;
    missing_active.read_active_blocks = {};
    EXPECT_EQ(nova::rpc::RpcService::Create(required, std::move(missing_active)), nullptr);
    auto throwing_guard = dependencies;
    throwing_guard.state_available = []() -> bool { throw 1; };
    auto throwing_service = nova::rpc::RpcService::Create(required, std::move(throwing_guard));
    ASSERT_NE(throwing_service, nullptr);
    EXPECT_EQ(throwing_service
                  ->HandleHttpPost({"POST", "Basic dXNlcjpwYXNz",
                                    R"({"jsonrpc":"2.0","id":4,"method":"getbalances"})"})
                  .status,
              500U);
    auto owner_only = dependencies;
    owner_only.chain_state = {};
    owner_only.utxos = {};
    owner_only.mempool = {};
    owner_only.wallet = {};
    auto legacy_config = required;
    legacy_config.require_node_mutation_callbacks = false;
    EXPECT_EQ(nova::rpc::RpcService::Create(legacy_config, owner_only), nullptr);
    auto owner_service = nova::rpc::RpcService::Create(required, std::move(owner_only));
    ASSERT_NE(owner_service, nullptr);
    auto service = nova::rpc::RpcService::Create(required, std::move(dependencies));
    ASSERT_NE(service, nullptr);
    const auto body = std::string{"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                                  "\"sendwallettransaction\",\"params\":{\"hex\":\""} +
                      hex + "\"}}";
    constexpr std::string_view auth{"Basic dXNlcjpwYXNz"};
    const auto encoded_address = nova::wallet::EncodeP2pkhAddress(
        address->public_key_hash, nova::consensus::RegtestNetworkParams());
    ASSERT_TRUE(encoded_address.has_value());
    const auto build_body = std::string{"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":"
                                        "\"createtransaction\",\"params\":{\"address\":\""} +
                            *encoded_address + "\",\"amount\":1}}";
    const auto sign_body = std::string{"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":"
                                       "\"signtransaction\",\"params\":{\"hex\":\""} +
                           hex + "\"}}";
    EXPECT_NE(owner_service
                  ->HandleHttpPost(
                      {"POST", auth, R"({"jsonrpc":"2.0","id":7,"method":"getblockchaininfo"})"})
                  .body.find("\"blocks\":0"),
              std::string::npos);
    EXPECT_NE(
        owner_service
            ->HandleHttpPost({"POST", auth, R"({"jsonrpc":"2.0","id":7,"method":"getbalances"})"})
            .body.find("\"confirmed\""),
        std::string::npos);
    EXPECT_EQ(owner_service->HandleHttpPost({"POST", auth, build_body}).body.find("\"error\""),
              std::string::npos);
    EXPECT_EQ(owner_service->HandleHttpPost({"POST", auth, sign_body}).body.find("\"error\""),
              std::string::npos);
    const auto address_count = wallet->ReceivingAddressCount();
    constexpr std::string_view address_body =
        R"({"jsonrpc":"2.0","id":2,"method":"getnewaddress"})";
    state_available = false;
    EXPECT_NE(
        service->HandleHttpPost({"POST", auth, address_body}).body.find("node state unavailable"),
        std::string::npos);
    EXPECT_NE(
        service
            ->HandleHttpPost({"POST", auth, R"({"jsonrpc":"2.0","id":3,"method":"getbalances"})"})
            .body.find("node state unavailable"),
        std::string::npos);
    EXPECT_EQ(address_calls, 0U);
    EXPECT_EQ(calls, 0U);
    state_available = true;
    constexpr std::string_view zero_hash =
        "0000000000000000000000000000000000000000000000000000000000000000";
    const auto block_query =
        std::string{
            "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"getblock\",\"params\":{\"hash\":\""} +
        std::string{zero_hash} + "\"}}";
    const auto transaction_query = std::string{"{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":"
                                               "\"getmempoolentry\",\"params\":{\"txid\":\""} +
                                   std::string{zero_hash} + "\"}}";
    const auto coin_query =
        std::string{
            "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"getutxo\",\"params\":{\"txid\":\""} +
        std::string{zero_hash} + "\",\"vout\":0}}";
    EXPECT_EQ(
        owner_service
            ->HandleHttpPost({"POST", auth, R"({"jsonrpc":"2.0","id":8,"method":"getnodehealth"})"})
            .body.find("\"error\""),
        std::string::npos);
    EXPECT_EQ(owner_service
                  ->HandleHttpPost(
                      {"POST", auth, R"({"jsonrpc":"2.0","id":8,"method":"getmempoolinfo"})"})
                  .body.find("\"error\""),
              std::string::npos);
    EXPECT_EQ(owner_service
                  ->HandleHttpPost(
                      {"POST", auth, R"({"jsonrpc":"2.0","id":8,"method":"getexplorersnapshot"})"})
                  .body.find("\"error\""),
              std::string::npos);
    EXPECT_NE(
        owner_service->HandleHttpPost({"POST", auth, block_query}).body.find("block not found"),
        std::string::npos);
    EXPECT_NE(owner_service->HandleHttpPost({"POST", auth, transaction_query})
                  .body.find("transaction not found"),
              std::string::npos);
    EXPECT_NE(owner_service->HandleHttpPost({"POST", auth, coin_query}).body.find("UTXO not found"),
              std::string::npos);
    EXPECT_NE(owner_service->HandleHttpPost({"POST", auth, body})
                  .body.find("transaction broadcast failed"),
              std::string::npos);
    EXPECT_EQ(calls, 1U);
    calls = 0U;
    lookup_available = false;
    EXPECT_NE(service
                  ->HandleHttpPost(
                      {"POST", auth, R"({"jsonrpc":"2.0","id":8,"method":"getblockchaininfo"})"})
                  .body.find("chain state unavailable"),
              std::string::npos);
    EXPECT_NE(
        service
            ->HandleHttpPost({"POST", auth, R"({"jsonrpc":"2.0","id":8,"method":"getnodehealth"})"})
            .body.find("chain state unavailable"),
        std::string::npos);
    EXPECT_NE(service
                  ->HandleHttpPost(
                      {"POST", auth, R"({"jsonrpc":"2.0","id":8,"method":"getmempoolinfo"})"})
                  .body.find("chain state unavailable"),
              std::string::npos);
    EXPECT_NE(service
                  ->HandleHttpPost(
                      {"POST", auth, R"({"jsonrpc":"2.0","id":8,"method":"getexplorersnapshot"})"})
                  .body.find("snapshot generation failed"),
              std::string::npos);
    EXPECT_NE(
        service->HandleHttpPost({"POST", auth, block_query}).body.find("block state unavailable"),
        std::string::npos);
    EXPECT_NE(service->HandleHttpPost({"POST", auth, transaction_query})
                  .body.find("mempool state unavailable"),
              std::string::npos);
    EXPECT_NE(
        service->HandleHttpPost({"POST", auth, coin_query}).body.find("UTXO state unavailable"),
        std::string::npos);
    lookup_available = true;
    EXPECT_NE(service
                  ->HandleHttpPost(
                      {"POST", auth, R"({"jsonrpc":"2.0","id":8,"method":"getblockchaininfo"})"})
                  .body.find("\"blocks\":0"),
              std::string::npos);
    EXPECT_NE(service->HandleHttpPost({"POST", auth, block_query}).body.find("block not found"),
              std::string::npos);
    EXPECT_NE(service->HandleHttpPost({"POST", auth, transaction_query})
                  .body.find("transaction not found"),
              std::string::npos);
    EXPECT_NE(service->HandleHttpPost({"POST", auth, coin_query}).body.find("UTXO not found"),
              std::string::npos);
    wallet_available = false;
    EXPECT_NE(
        service
            ->HandleHttpPost({"POST", auth, R"({"jsonrpc":"2.0","id":3,"method":"getbalances"})"})
            .body.find("wallet state unavailable"),
        std::string::npos);
    EXPECT_NE(
        service
            ->HandleHttpPost({"POST", auth, R"({"jsonrpc":"2.0","id":3,"method":"listunspent"})"})
            .body.find("wallet state unavailable"),
        std::string::npos);
    EXPECT_NE(
        service->HandleHttpPost({"POST", auth, build_body}).body.find("wallet state unavailable"),
        std::string::npos);
    EXPECT_NE(
        service->HandleHttpPost({"POST", auth, sign_body}).body.find("wallet state unavailable"),
        std::string::npos);
    wallet_available = true;
    EXPECT_EQ(service->HandleHttpPost({"POST", auth, build_body}).body.find("\"error\""),
              std::string::npos);
    EXPECT_EQ(service->HandleHttpPost({"POST", auth, sign_body}).body.find("\"error\""),
              std::string::npos);
    verification = std::nullopt;
    EXPECT_NE(service->HandleHttpPost({"POST", auth, body}).body.find("wallet state unavailable"),
              std::string::npos);
    EXPECT_EQ(calls, 0U);
    verification = false;
    EXPECT_NE(
        service->HandleHttpPost({"POST", auth, body}).body.find("local signature check failed"),
        std::string::npos);
    EXPECT_EQ(calls, 0U);
    verification = true;
    EXPECT_NE(service->HandleHttpPost({"POST", auth, address_body})
                  .body.find("address generation failed"),
              std::string::npos);
    EXPECT_EQ(address_calls, 1U);
    EXPECT_EQ(wallet->ReceivingAddressCount(), address_count);
    EXPECT_EQ(persist_calls, 0U);
    address_success = true;
    EXPECT_EQ(service->HandleHttpPost({"POST", auth, address_body}).body.find("\"error\""),
              std::string::npos);
    EXPECT_EQ(address_calls, 2U);
    EXPECT_EQ(wallet->ReceivingAddressCount(), address_count);
    EXPECT_EQ(persist_calls, 0U);
    EXPECT_NE(
        service->HandleHttpPost({"POST", auth, body}).body.find("transaction broadcast failed"),
        std::string::npos);
    EXPECT_EQ(calls, 1U);
    EXPECT_EQ(mempool->size(), 0U);
    accept = true;
    EXPECT_EQ(service->HandleHttpPost({"POST", auth, body}).body.find("\"error\""),
              std::string::npos);
    EXPECT_EQ(calls, 2U);
    EXPECT_EQ(mempool->size(), 0U);
    lose_authority = true;
    const auto unknown = "operation outcome may have committed";
    EXPECT_NE(service->HandleHttpPost({"POST", auth, address_body}).body.find(unknown),
              std::string::npos);
    state_available = true;
    const auto uncertain_submission = service->HandleHttpPost({"POST", auth, body});
    EXPECT_NE(uncertain_submission.body.find(unknown), std::string::npos);
    const auto submitted_id = built.built->transaction.TxId(kTransactionLimits);
    ASSERT_TRUE(submitted_id);
    std::string id_hex;
    for (const auto byte : submitted_id->bytes()) {
        id_hex.push_back(digits[byte >> 4U]);
        id_hex.push_back(digits[byte & 15U]);
    }
    EXPECT_NE(uncertain_submission.body.find(id_hex), std::string::npos);
    state_available = true;
    const auto send_body = std::string{"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"sendtoaddress\","
                                       "\"params\":{\"address\":\""} +
                           *encoded_address + "\",\"amount\":1}}";
    EXPECT_NE(service->HandleHttpPost({"POST", auth, send_body}).body.find(unknown),
              std::string::npos);
    state_available = true;
    EXPECT_NE(service
                  ->HandleHttpPost({"POST", auth,
                                    R"({"jsonrpc":"2.0","id":10,"method":"generateregtestblock"})"})
                  .body.find(unknown),
              std::string::npos);
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

    const auto snapshot = disabled->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":7,"method":"getexplorersnapshot"})"});
    EXPECT_EQ(snapshot.status, 200U);
    EXPECT_NE(snapshot.body.find("nova-snapshot-v1"), std::string::npos);

    const auto testnet_mining = disabled->HandleHttpPost(
        {"POST", kAuth, R"({"jsonrpc":"2.0","id":8,"method":"generateregtestblock"})"});
    EXPECT_NE(testnet_mining.body.find("-32005"), std::string::npos);
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
    nova::crypto::Hash160 recipient_hash{};
    const auto recipient =
        nova::wallet::EncodeP2pkhAddress(recipient_hash, nova::consensus::RegtestNetworkParams());
    ASSERT_TRUE(recipient.has_value());
    const auto sent = service->HandleHttpPost(
        {"POST", kAuth,
         "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"sendtoaddress\",\"params\":{\"address\":\"" +
             *recipient + "\",\"amount\":1}}"});
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

TEST(ExplorerRpcSnapshotIntegration, ExportsSelectedGenesisAndRejectsOtherNetworkWithoutMutation)
{
    for (const auto* network :
         {&nova::consensus::RegtestNetworkParams(), &nova::consensus::TestnetNetworkParams()}) {
        auto state = nova::chain::NetworkChainInitializer::Create(
            network->id, std::make_shared<nova::chain::FixedValidationTimeSource>(1'704'160'000U));
        ASSERT_TRUE(state.has_value());
        const auto validation = nova::chain::NetworkValidationParams(*network);
        auto mempool = Mempool::Create(MempoolParams{validation, 16U, 100'000U, 0});
        auto wallet = nova::wallet::Wallet::Create(
            nova::wallet::WalletParams{network->block_limits.transaction_limits, 1, 1U});
        auto peers = MakePeerManager();
        ASSERT_NE(mempool, nullptr);
        ASSERT_NE(wallet, nullptr);
        nova::rpc::RpcDependencies dependencies{*state->chain,
                                                *state->utxos,
                                                *mempool,
                                                peers,
                                                *wallet,
                                                {1U, 1'704'160'000U},
                                                1'704'160'000U,
                                                {},
                                                {},
                                                {},
                                                {},
                                                {},
                                                {}};
        auto service = nova::rpc::RpcService::Create(
            {"127.0.0.1", "user", "pass", {256U, 8'192U, 4'096U}, network->id},
            std::move(dependencies));
        ASSERT_NE(service, nullptr);
        auto server = nova::rpc::LoopbackHttpServer::Create(
            {"127.0.0.1", 0U, 4U, 256U, 8'192U, 30U}, *service);
        ASSERT_NE(server, nullptr);
        const auto source = nova::explorer::AuthenticatedRpcSnapshotSource::Create(
            {"127.0.0.1", server->port(), "user", "pass", 1U * 1024U * 1024U, network});
        const auto* other = network->id == nova::consensus::NetworkId::kTestnet
                                ? &nova::consensus::RegtestNetworkParams()
                                : &nova::consensus::TestnetNetworkParams();
        const auto wrong = nova::explorer::AuthenticatedRpcSnapshotSource::Create(
            {"127.0.0.1", server->port(), "user", "pass", 1U * 1024U * 1024U, other});
        ASSERT_NE(source, nullptr);
        ASSERT_NE(wrong, nullptr);
        const auto tip = state->chain->active_tip();
        const auto count = state->utxos->size();
        std::atomic<bool> running{true};
        std::thread pump{[&server, &running]() {
            while (running.load()) {
                static_cast<void>(server->Pump(1U));
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }};
        const auto snapshot = source->ReadSnapshot();
        const auto rejected = wrong->ReadSnapshot();
        running.store(false);
        pump.join();
        ASSERT_TRUE(snapshot.has_value());
        ASSERT_EQ(snapshot->active_blocks.size(), 1U);
        EXPECT_EQ(snapshot->network, network->id);
        EXPECT_EQ(snapshot->active_blocks.front().hash, network->genesis_hash);
        nova::primitives::BinaryWriter actual, expected;
        ASSERT_TRUE(snapshot->active_blocks.front().block.Serialize(actual, network->block_limits));
        ASSERT_TRUE(network->genesis_block.Serialize(expected, network->block_limits));
        EXPECT_TRUE(std::ranges::equal(actual.bytes(), expected.bytes()));
        EXPECT_FALSE(rejected.has_value());
        nova::explorer::ExplorerIndex index;
        EXPECT_EQ(index.Rebuild(*snapshot).error, nova::explorer::ExplorerError::kNone);
        EXPECT_EQ(state->chain->active_tip(), tip);
        EXPECT_EQ(state->utxos->size(), count);
        EXPECT_EQ(mempool->size(), 0U);
    }
    EXPECT_TRUE(nova::consensus::TestnetNetworkParams().enabled);
    EXPECT_FALSE(nova::consensus::MainnetNetworkParams().enabled);
}

TEST(RpcReadFailure, GetUtxoUnavailableIsNotNotFoundAndParamsDoNotReadState)
{
    UTXOSet utxos;
    auto state = CreateChainState(utxos);
    auto pool = Mempool::Create(MempoolParams{RegtestValidationParams(), 16U, 100000U, 0});
    auto peers = MakePeerManager();
    auto wallet = nova::wallet::Wallet::Create({kTransactionLimits, 1, 1U});
    ASSERT_NE(state, nullptr);
    ASSERT_NE(pool, nullptr);
    ASSERT_NE(wallet, nullptr);
    const nova::chain::UTXOKey key{AnchorHash(), 0U};
    ASSERT_TRUE(utxos.AddCoin(
        key, nova::chain::Coin{nova::primitives::TxOutput{COIN, {0x51U}}, 0U, false}));
    nova::rpc::RpcDependencies dependencies{
        *state, utxos, *pool, peers, *wallet, {1U, 1000U}, 1000U, {}, {}, {}, {}, {}, {}};
    auto service = nova::rpc::RpcService::Create(
        {"127.0.0.1", "user", "pass", {256U, 8192U, 4096U}, nova::consensus::NetworkId::kRegtest},
        std::move(dependencies));
    ASSERT_NE(service, nullptr);
    constexpr std::string_view auth{"Basic dXNlcjpwYXNz"};
    const auto prefix =
        std::string{
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getutxo\",\"params\":{\"txid\":\""} +
        (std::string(63U, '0') + "1") + "\",\"vout\":";
    const auto good = prefix + "0}}";
    utxos.FailNextReadForTesting();
    const auto invalid = service->HandleHttpPost({"POST", auth, prefix + "-1}}"});
    EXPECT_NE(invalid.body.find("-32602"), std::string::npos);
    const auto unavailable = service->HandleHttpPost({"POST", auth, good});
    EXPECT_NE(unavailable.body.find("-32004"), std::string::npos);
    EXPECT_NE(unavailable.body.find("UTXO state unavailable"), std::string::npos);
    EXPECT_EQ(unavailable.body.find("UTXO not found"), std::string::npos);
    const auto healthy = service->HandleHttpPost({"POST", auth, good});
    EXPECT_NE(healthy.body.find("\"result\":"), std::string::npos);
    const auto missing = service->HandleHttpPost({"POST", auth, prefix + "1}}"});
    EXPECT_NE(missing.body.find("-32002"), std::string::npos);
    EXPECT_EQ(utxos.size(), 1U);
    EXPECT_TRUE(utxos.HaveCoin(key));
}

} // namespace
