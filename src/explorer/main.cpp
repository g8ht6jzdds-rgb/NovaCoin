#include "explorer/explorer.hpp"
#include "explorer/http.hpp"
#include "explorer/http_server.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace
{

[[nodiscard]] std::optional<std::uint16_t> Port(const std::string_view text)
{
    std::uint16_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value != 0U
               ? std::optional<std::uint16_t>{value}
               : std::nullopt;
}

[[nodiscard]] std::uint64_t NowSeconds() noexcept
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return seconds <= 0 ? 0U : static_cast<std::uint64_t>(seconds);
}

[[nodiscard]] const char* NetworkName(const nova::consensus::NetworkId network) noexcept
{
    switch (network) {
    case nova::consensus::NetworkId::kRegtest:
        return "regtest";
    case nova::consensus::NetworkId::kTestnet:
        return "testnet";
    case nova::consensus::NetworkId::kMainnet:
        return "mainnet";
    }
    return "invalid";
}

[[nodiscard]] std::optional<std::string> EnvironmentSecret(const char* const name)
{
#ifdef _WIN32
    char* value{};
    std::size_t length{};
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr || value[0] == '\0') {
        if (value != nullptr) {
            std::fill_n(value, length, '\0');
            // `_dupenv_s` pairs with `free`; erase the duplicate before release.
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
            std::free(value);
        }
        return std::nullopt;
    }
    std::string result{value};
    std::fill_n(value, length, '\0');
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    std::free(value);
    return result;
#else
    const char* const value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string{value};
#endif
}

} // namespace

int main(const int argc, char* argv[])
{
    std::optional<std::uint16_t> rpc_port;
    std::optional<std::uint16_t> http_port;
    std::optional<nova::consensus::NetworkId> network;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--regtest") {
            if (network.has_value()) {
                std::cerr << "duplicate network selection\n";
                return 2;
            }
            network = nova::consensus::NetworkId::kRegtest;
            continue;
        }
        if (option == "--testnet") {
            if (network.has_value()) {
                std::cerr << "duplicate network selection\n";
                return 2;
            }
            network = nova::consensus::NetworkId::kTestnet;
            continue;
        }
        if (++index >= argc) {
            std::cerr << "missing option value\n";
            return 2;
        }
        const std::string_view value{argv[index]};
        if (option == "--rpcport" && !rpc_port.has_value()) {
            rpc_port = Port(value);
        } else if (option == "--httpport" && !http_port.has_value()) {
            http_port = Port(value);
        } else {
            std::cerr << "invalid or duplicate option\n";
            return 2;
        }
    }
    if (!network.has_value() || !rpc_port.has_value() || !http_port.has_value() ||
        *rpc_port == *http_port) {
        std::cerr
            << "usage: nova-explorer --regtest|--testnet --rpcport <port> --httpport <port>\n";
        return 2;
    }
    const auto& parameters = nova::consensus::GetNetworkParams(*network);
    if (nova::consensus::CheckNetworkParams(parameters) !=
            nova::consensus::NetworkParamsError::kNone ||
        !parameters.enabled) {
        std::cerr << NetworkName(*network) << " is disabled pending immutable parameter approval\n";
        return 1;
    }
    const auto rpc_password = EnvironmentSecret("NOVACOIN_RPC_PASSWORD");
    const auto explorer_password = EnvironmentSecret("NOVACOIN_EXPLORER_PASSWORD");
    if (!rpc_password.has_value() || !explorer_password.has_value()) {
        std::cerr << "NOVACOIN_RPC_PASSWORD and NOVACOIN_EXPLORER_PASSWORD must be supplied out of "
                     "band\n";
        return 2;
    }

    const auto source = nova::explorer::AuthenticatedRpcSnapshotSource::Create(
        {"127.0.0.1", *rpc_port, "novacoin", *rpc_password, 32U * 1024U * 1024U, &parameters});
    if (source == nullptr) {
        std::cerr << "explorer RPC source initialization failed\n";
        return 1;
    }
    nova::explorer::ExplorerIndex index;
    const auto service = nova::explorer::ExplorerHttpService::Create(
        {"127.0.0.1", "explorer", *explorer_password, {1'024U, 1'024U, 100U}}, index);
    const auto server =
        service == nullptr
            ? nullptr
            : nova::explorer::ExplorerLoopbackHttpServer::Create(
                  {"127.0.0.1", *http_port, 32U, 8'192U, 1'024U, 4U * 1024U * 1024U, 30U},
                  *service);
    if (server == nullptr) {
        std::cerr << "explorer HTTP initialization failed\n";
        return 1;
    }

    std::uint64_t next_refresh{};
    std::cout << "nova-explorer " << NetworkName(*network) << " read-only service ready\n";
    while (true) {
        const auto now = NowSeconds();
        if (now >= next_refresh) {
            const auto snapshot = source->ReadSnapshot();
            if (snapshot.has_value() &&
                index.Rebuild(*snapshot).error != nova::explorer::ExplorerError::kNone) {
                std::cerr << "explorer rejected node snapshot\n";
            }
            next_refresh = now + 2U;
        }
        static_cast<void>(server->Pump(now));
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}
