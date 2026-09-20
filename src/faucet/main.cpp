#include "consensus/network_params.hpp"
#include "faucet/faucet.hpp"
#include "faucet/http.hpp"
#include "faucet/http_server.hpp"
#include "faucet/rpc_client.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace
{

std::atomic_bool shutdown_requested{false};

void RequestShutdown(int) noexcept
{
    shutdown_requested.store(true);
}

[[nodiscard]] std::optional<std::string> EnvironmentSecret(const char* const name)
{
#ifdef _WIN32
    char* value{};
    std::size_t length{};
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr || length <= 1U) {
        if (value != nullptr) {
            std::fill_n(value, length, '\0');
            // `_dupenv_s` pairs with `free`; erase the duplicate before release.
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
            std::free(value);
        }
        return std::nullopt;
    }
    std::string result{value, length - 1U};
    std::fill_n(value, length, '\0');
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    std::free(value);
    return result;
#else
    const char* const value = std::getenv(name);
    return value == nullptr || value[0] == '\0' ? std::nullopt : std::optional<std::string>{value};
#endif
}

template <typename Value>
[[nodiscard]] std::optional<Value> UnsignedEnvironment(const char* const name)
{
    const auto value = EnvironmentSecret(name);
    if (!value.has_value() || value->empty()) {
        return std::nullopt;
    }
    Value parsed{};
    const auto result = std::from_chars(value->data(), value->data() + value->size(), parsed);
    return result.ec == std::errc{} && result.ptr == value->data() + value->size() && parsed > 0
               ? std::optional<Value>{parsed}
               : std::nullopt;
}

[[nodiscard]] std::optional<std::uint16_t> Port(const std::string_view value)
{
    std::uint16_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result != 0U
               ? std::optional<std::uint16_t>{result}
               : std::nullopt;
}

[[nodiscard]] std::uint64_t NowSeconds() noexcept
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0U;
}

} // namespace

int main(const int argc, char* argv[])
{
    std::optional<std::uint16_t> public_adapter_port;
    std::optional<std::uint16_t> rpc_port;
    std::optional<std::filesystem::path> audit_log;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (++index >= argc) {
            std::cerr << "missing option value\n";
            return 2;
        }
        const std::string_view value{argv[index]};
        if (option == "--port" && !public_adapter_port.has_value()) {
            public_adapter_port = Port(value);
        } else if (option == "--rpcport" && !rpc_port.has_value()) {
            rpc_port = Port(value);
        } else if (option == "--auditlog" && !audit_log.has_value() && !value.empty()) {
            audit_log = std::filesystem::path{value};
        } else {
            std::cerr << "invalid or duplicate option\n";
            return 2;
        }
    }
    if (!public_adapter_port.has_value() || !rpc_port.has_value() || !audit_log.has_value() ||
        *public_adapter_port == *rpc_port) {
        std::cerr << "usage: novacoin-faucet --port <loopback-port> --rpcport <node-rpc-port> "
                     "--auditlog <path>\n";
        return 2;
    }

    const auto& network = nova::consensus::TestnetNetworkParams();
    if (!network.enabled || network.id != nova::consensus::NetworkId::kTestnet ||
        nova::consensus::CheckNetworkParams(network) !=
            nova::consensus::NetworkParamsError::kNone) {
        std::cerr << "TESTNET faucet is disabled pending immutable parameter approval\n";
        return 1;
    }
    const auto rpc_password = EnvironmentSecret("NOVACOIN_FAUCET_RPC_PASSWORD");
    const auto payout =
        UnsignedEnvironment<nova::primitives::Amount>("NOVACOIN_FAUCET_PAYOUT_AMOUNT");
    const auto maximum =
        UnsignedEnvironment<nova::primitives::Amount>("NOVACOIN_FAUCET_MAX_PAYOUT");
    const auto window = UnsignedEnvironment<std::uint64_t>("NOVACOIN_FAUCET_SOURCE_WINDOW_SECONDS");
    const auto requests =
        UnsignedEnvironment<std::uint32_t>("NOVACOIN_FAUCET_MAX_REQUESTS_PER_WINDOW");
    const auto sources = UnsignedEnvironment<std::size_t>("NOVACOIN_FAUCET_MAX_TRACKED_SOURCES");
    const auto source_bytes =
        UnsignedEnvironment<std::size_t>("NOVACOIN_FAUCET_MAX_SOURCE_IDENTIFIER_BYTES");
    const auto address_window =
        UnsignedEnvironment<std::uint64_t>("NOVACOIN_FAUCET_ADDRESS_WINDOW_SECONDS");
    const auto address_requests =
        UnsignedEnvironment<std::uint32_t>("NOVACOIN_FAUCET_MAX_REQUESTS_PER_ADDRESS_WINDOW");
    const auto addresses =
        UnsignedEnvironment<std::size_t>("NOVACOIN_FAUCET_MAX_TRACKED_ADDRESSES");
    const auto global_window =
        UnsignedEnvironment<std::uint64_t>("NOVACOIN_FAUCET_GLOBAL_WINDOW_SECONDS");
    const auto global_payout = UnsignedEnvironment<nova::primitives::Amount>(
        "NOVACOIN_FAUCET_MAX_PAYOUT_PER_GLOBAL_WINDOW");
    if (!rpc_password.has_value() || !payout.has_value() || !maximum.has_value() ||
        !window.has_value() || !requests.has_value() || !sources.has_value() ||
        !source_bytes.has_value() || !address_window.has_value() || !address_requests.has_value() ||
        !addresses.has_value() || !global_window.has_value() || !global_payout.has_value() ||
        *maximum < *payout || *global_payout < *payout) {
        std::cerr << "missing or invalid faucet configuration\n";
        return 2;
    }
    const auto client = nova::faucet::FaucetRpcClient::Create(
        {"127.0.0.1", *rpc_port, "faucet", *rpc_password, 16U * 1024U, &network});
    if (client == nullptr) {
        std::cerr << "restricted faucet RPC client initialization failed\n";
        return 1;
    }
    const auto service = nova::faucet::FaucetService::Create(
        network,
        {*payout, *maximum, *window, *requests, *sources, *source_bytes, *address_window,
         *address_requests, *addresses, *global_window, *global_payout},
        *audit_log,
        [&client](const nova::wallet::Recipient& recipient) { return client->Pay(recipient); });
    const auto adapter = service == nullptr ? nullptr
                                            : nova::faucet::FaucetHttpService::Create(
                                                  {"127.0.0.1", {1'024U, *source_bytes}}, *service);
    const auto server =
        adapter == nullptr
            ? nullptr
            : nova::faucet::FaucetLoopbackHttpServer::Create(
                  {"127.0.0.1", *public_adapter_port, 32U, 8'192U, 1'024U, 16U * 1024U, 30U},
                  *adapter);
    if (server == nullptr) {
        std::cerr << "faucet loopback HTTP initialization failed\n";
        return 1;
    }
    static_cast<void>(std::signal(SIGINT, RequestShutdown));
    static_cast<void>(std::signal(SIGTERM, RequestShutdown));
    std::cout << "novacoin-faucet TESTNET loopback adapter ready\n";
    while (!shutdown_requested.load()) {
        static_cast<void>(server->Pump(NowSeconds()));
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    server->Close();
    std::cout << "novacoin-faucet shutdown complete\n";
    return 0;
}
