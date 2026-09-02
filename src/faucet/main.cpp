#include "consensus/network_params.hpp"
#include "faucet/faucet.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace
{

[[nodiscard]] std::optional<std::string> EnvironmentSecret(const char* name)
{
#ifdef _WIN32
    char* value{};
    std::size_t length{};
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr || length <= 1U) {
        return std::nullopt;
    }
    std::string result{value, length - 1U};
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? std::nullopt : std::optional<std::string>{value};
#endif
}

} // namespace

int main()
{
    // The public request adapter is intentionally deployed separately from
    // novacoind. This process refuses to start until the TESTNET gate is
    // enabled and an out-of-band faucet RPC credential is supplied. It never
    // exposes or writes the node wallet's private keys.
    const auto& network = nova::consensus::TestnetNetworkParams();
    if (!network.enabled || network.id != nova::consensus::NetworkId::kTestnet ||
        !EnvironmentSecret("NOVACOIN_FAUCET_RPC_PASSWORD").has_value()) {
        std::cerr << "TESTNET faucet is disabled or lacks a restricted RPC credential\n";
        return 1;
    }
    std::cerr << "novacoin-faucet requires a separately configured restricted payout client\n";
    return 1;
}
