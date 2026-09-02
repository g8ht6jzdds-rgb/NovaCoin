#pragma once

#include "chain/chainstate.hpp"
#include "chain/mempool.hpp"
#include "net/p2p.hpp"
#include "net/transport.hpp"
#include "observability/metrics.hpp"
#include "wallet/wallet.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nova::rpc
{

struct RpcLimits final {
    std::size_t max_header_bytes{};
    std::size_t max_body_bytes{};
    std::uint64_t max_mining_attempts{};
};

// The administrator credential retains normal node and wallet authority.
// Restricted principals have an allow-list enforced before method dispatch so
// an explorer or faucet process cannot inherit administrative capabilities.
enum class RpcRole : std::uint8_t { kExplorer, kFaucet };

struct RpcRestrictedPrincipal final {
    std::string username;
    std::string password;
    RpcRole role{RpcRole::kExplorer};
};

struct RpcConfig final {
    std::string bind_address{"127.0.0.1"};
    std::string username;
    std::string password;
    RpcLimits limits;
    consensus::NetworkId network{consensus::NetworkId::kRegtest};
    std::vector<RpcRestrictedPrincipal> restricted_principals;
    primitives::Amount maximum_faucet_payout{};

    RpcConfig() = default;
    RpcConfig(std::string bind, std::string administrator, std::string secret, RpcLimits rpc_limits,
              const consensus::NetworkId selected_network)
        : bind_address(std::move(bind)), username(std::move(administrator)),
          password(std::move(secret)), limits(rpc_limits), network(selected_network)
    {
    }
};

struct RpcDependencies final {
    chain::ChainState& chain_state;
    chain::UTXOSet& utxos;
    chain::Mempool& mempool;
    net::PeerManager& peers;
    wallet::Wallet& wallet;
    chain::TransactionValidationContext transaction_context;
    std::uint64_t current_time{};
    std::function<bool()> persist_wallet;
    // Regtest-only application callbacks. They enter the ordinary node
    // pipeline; RPC itself never accepts a block or mutates consensus state.
    std::function<std::optional<crypto::Hash256>()> mine_regtest_block;
    std::function<std::optional<crypto::Hash256>(const wallet::Recipient&)> send_to_address;
    std::function<bool(const net::TcpEndpoint&)> connect_peer;
    std::function<void()> disconnect_peers;
    // Read-only process-local telemetry supplied by the daemon. It has no
    // authority over consensus state and is optional for embedders.
    std::function<observability::MetricsSnapshot()> read_metrics;
};

struct HttpRequest final {
    std::string_view method;
    std::string_view authorization;
    std::string_view body;
};

struct HttpResponse final {
    std::uint16_t status{};
    std::string body;
};

struct HttpServerParams final {
    std::string bind_address{"127.0.0.1"};
    std::uint16_t port{};
    std::size_t max_connections{};
    std::size_t max_header_bytes{};
    std::size_t max_body_bytes{};
    std::uint64_t idle_timeout_seconds{};
};

enum class HttpServerError : std::uint8_t {
    kNone,
    kInvalidParameters,
    kSocketFailure,
    kBindFailure,
    kListenFailure,
    kAllocationFailure,
};

struct HttpServerResult final {
    HttpServerError error{HttpServerError::kNone};
    std::size_t accepted{};
    std::size_t completed{};
    std::size_t rejected{};
};

// A loopback HTTP listener or daemon transport calls HandleHttpPost after it
// has bounded the HTTP framing.  This class owns no consensus state.
class RpcService final
{
  public:
    RpcService(const RpcService&) = delete;
    RpcService& operator=(const RpcService&) = delete;

    [[nodiscard]] static std::unique_ptr<RpcService> Create(RpcConfig config,
                                                            RpcDependencies dependencies) noexcept;
    [[nodiscard]] HttpResponse HandleHttpPost(const HttpRequest& request) noexcept;
    [[nodiscard]] const std::string& bind_address() const noexcept;

  private:
    RpcService(RpcConfig config, RpcDependencies dependencies) noexcept;

    RpcConfig config_;
    RpcDependencies dependencies_;
};

// A small, deliberately one-request-per-connection HTTP/1.1 adapter. It is
// loopback-only, enforces framing limits before allocating request bodies, and
// delegates authentication and JSON-RPC handling to RpcService.
class LoopbackHttpServer final
{
  public:
    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer(LoopbackHttpServer&&) = delete;
    LoopbackHttpServer& operator=(LoopbackHttpServer&&) = delete;
    ~LoopbackHttpServer();

    [[nodiscard]] static std::unique_ptr<LoopbackHttpServer> Create(HttpServerParams parameters,
                                                                    RpcService& service) noexcept;
    [[nodiscard]] HttpServerResult Pump(std::uint64_t now) noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    void Close() noexcept;

  private:
    struct Impl;
    explicit LoopbackHttpServer(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace nova::rpc
