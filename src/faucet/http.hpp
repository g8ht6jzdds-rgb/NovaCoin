#pragma once

#include "faucet/faucet.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace nova::faucet
{

struct FaucetHttpLimits final {
    std::size_t max_body_bytes{};
    std::size_t max_source_identifier_bytes{};
};

struct FaucetHttpConfig final {
    std::string bind_address{"127.0.0.1"};
    FaucetHttpLimits limits;
};

struct FaucetHttpRequest final {
    std::string_view method;
    std::string_view target;
    std::string_view body;
    // This is derived from the accepted TCP peer by FaucetLoopbackHttpServer;
    // it is never accepted from the JSON request body.
    std::string_view source_identifier;
    std::uint64_t received_at{};
};

struct FaucetHttpResponse final {
    std::uint16_t status{};
    std::string content_type{"application/json"};
    std::string body;
};

// This request adapter is deliberately separate from novacoind and owns no
// consensus, wallet, or RPC state. It only converts a bounded HTTP request to
// FaucetService, whose payout callback is the restricted TESTNET faucet RPC.
class FaucetHttpService final
{
  public:
    FaucetHttpService(const FaucetHttpService&) = delete;
    FaucetHttpService& operator=(const FaucetHttpService&) = delete;

    [[nodiscard]] static std::unique_ptr<FaucetHttpService> Create(FaucetHttpConfig config,
                                                                   FaucetService& service) noexcept;
    [[nodiscard]] FaucetHttpResponse Handle(const FaucetHttpRequest& request) noexcept;

  private:
    FaucetHttpService(FaucetHttpConfig config, FaucetService& service) noexcept;

    FaucetHttpConfig config_;
    FaucetService& service_;
};

} // namespace nova::faucet
