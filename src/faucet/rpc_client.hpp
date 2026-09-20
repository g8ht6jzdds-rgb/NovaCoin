#pragma once

#include "consensus/network_params.hpp"
#include "crypto/crypto.hpp"
#include "wallet/wallet.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nova::faucet
{

// Configuration for the faucet's narrowly-scoped, authenticated local RPC
// client.  The client never accepts a remote host: an ingress service must
// not be able to redirect faucet spending authority to another endpoint.
struct FaucetRpcClientConfig final {
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
    std::string username{"faucet"};
    std::string password;
    std::size_t maximum_response_bytes{};
    const consensus::NetworkParams* network{};
};

class FaucetRpcClient final
{
  public:
    FaucetRpcClient(const FaucetRpcClient&) = delete;
    FaucetRpcClient& operator=(const FaucetRpcClient&) = delete;

    [[nodiscard]] static std::unique_ptr<FaucetRpcClient>
    Create(FaucetRpcClientConfig configuration) noexcept;

    // Issues only the restricted `faucetpay` RPC.  Any transport, HTTP,
    // JSON-RPC, response-size, or transaction-ID parsing failure is reported
    // as no result; the caller fails the public request closed.
    [[nodiscard]] std::optional<crypto::Hash256>
    Pay(const wallet::Recipient& recipient) const noexcept;

  private:
    explicit FaucetRpcClient(FaucetRpcClientConfig configuration) noexcept;

    FaucetRpcClientConfig configuration_;
};

} // namespace nova::faucet
