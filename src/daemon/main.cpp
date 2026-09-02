#include "node/bootstrap_config.hpp"
#include "node/network_selection.hpp"
#include "node/peer_service.hpp"
#include "node/regtest_node.hpp"
#include "rpc/rpc.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

volatile std::sig_atomic_t shutdown_requested = 0;

void RequestShutdown(int) noexcept
{
    // A signal handler may only perform async-signal-safe work.  The ordinary
    // event loop performs all listener shutdown and wallet persistence.
    shutdown_requested = 1;
}

[[nodiscard]] std::optional<std::uint16_t> Port(const std::string_view text)
{
    std::uint16_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value != 0U
               ? std::optional<std::uint16_t>{value}
               : std::nullopt;
}

[[nodiscard]] std::optional<nova::net::TcpEndpoint> Endpoint(const std::string_view text)
{
    const auto separator = text.rfind(':');
    if (separator == std::string_view::npos || separator == 0U || separator + 1U >= text.size()) {
        return std::nullopt;
    }
    const auto port = Port(text.substr(separator + 1U));
    if (!port.has_value() || separator > 255U) {
        return std::nullopt;
    }
    return nova::net::TcpEndpoint{std::string{text.substr(0U, separator)}, *port};
}

[[nodiscard]] bool IsPermittedP2pBindAddress(const std::string_view address) noexcept
{
    return address == "127.0.0.1" || address == "::1" || address == "0.0.0.0" || address == "::";
}

[[nodiscard]] bool IsWildcardP2pBindAddress(const std::string_view address) noexcept
{
    return address == "0.0.0.0" || address == "::";
}

[[nodiscard]] std::uint64_t NowSeconds() noexcept
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return seconds <= 0 ? 0U : static_cast<std::uint64_t>(seconds);
}

#ifdef _WIN32
void ReleaseSecretEnvironmentString(char* value, const std::size_t size) noexcept
{
    if (value == nullptr) {
        return;
    }
    std::fill_n(value, size, '\0');
    // `_dupenv_s` allocates with the C runtime and requires `free`; isolate
    // this API boundary and erase the duplicated secret first.
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    std::free(value);
}
#endif

[[nodiscard]] std::optional<std::string> EnvironmentValue(const char* name)
{
#ifdef _WIN32
    char* value{};
    std::size_t length{};
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result{value, length == 0U ? 0U : length - 1U};
    ReleaseSecretEnvironmentString(value, length);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::nullopt : std::optional<std::string>{value};
#endif
}

} // namespace

int main(const int argc, char* argv[])
{
    static_cast<void>(std::signal(SIGINT, RequestShutdown));
    static_cast<void>(std::signal(SIGTERM, RequestShutdown));
    std::optional<std::string> name;
    std::optional<std::filesystem::path> data_directory;
    std::optional<std::filesystem::path> log_path;
    std::optional<std::uint16_t> p2p_port;
    std::optional<std::uint16_t> rpc_port;
    std::optional<std::string> p2p_bind_address;
    std::vector<nova::net::TcpEndpoint> peers;
    std::optional<std::filesystem::path> bootstrap_config;
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
        if (option == "--mainnet") {
            if (network.has_value()) {
                std::cerr << "duplicate network selection\n";
                return 2;
            }
            network = nova::consensus::NetworkId::kMainnet;
            continue;
        }
        if (++index >= argc) {
            std::cerr << "missing option value\n";
            return 2;
        }
        const std::string_view value{argv[index]};
        if (option == "--name" && !name.has_value()) {
            name = value;
        } else if (option == "--datadir" && !data_directory.has_value()) {
            data_directory = std::filesystem::path{value};
        } else if (option == "--p2pport" && !p2p_port.has_value()) {
            p2p_port = Port(value);
        } else if (option == "--p2pbind" && !p2p_bind_address.has_value() &&
                   IsPermittedP2pBindAddress(value)) {
            p2p_bind_address = std::string{value};
        } else if (option == "--rpcport" && !rpc_port.has_value()) {
            rpc_port = Port(value);
        } else if (option == "--logfile" && !log_path.has_value()) {
            log_path = std::filesystem::path{value};
        } else if (option == "--connect") {
            const auto endpoint = Endpoint(value);
            if (!endpoint.has_value()) {
                std::cerr << "invalid --connect endpoint\n";
                return 2;
            }
            peers.push_back(*endpoint);
        } else if (option == "--bootstrap" && !bootstrap_config.has_value()) {
            bootstrap_config = std::filesystem::path{value};
        } else {
            std::cerr << "invalid or duplicate option\n";
            return 2;
        }
    }
    if (!network.has_value() || !name.has_value() || !data_directory.has_value() ||
        !log_path.has_value() || !p2p_port.has_value() || !rpc_port.has_value() ||
        *p2p_port == *rpc_port) {
        std::cerr
            << "usage: novacoind --regtest|--testnet|--mainnet --name <name> --datadir <path> "
               "--p2pport <port> --rpcport <port> --logfile <path> [--p2pbind loopback|wildcard] "
               "[--connect host:port] [--bootstrap static-bootstrap.conf]\n";
        return 2;
    }
    const auto selection = nova::node::SelectNetwork(*network);
    if (selection.error != nova::node::NetworkSelectionError::kNone ||
        selection.parameters == nullptr) {
        std::cerr << nova::node::NetworkName(*network)
                  << " is disabled pending immutable parameter approval\n";
        return 1;
    }
    const auto p2p_bind = p2p_bind_address.value_or("127.0.0.1");
    if (IsWildcardP2pBindAddress(p2p_bind) && *network != nova::consensus::NetworkId::kTestnet) {
        std::cerr << "public P2P binding is permitted only for TESTNET\n";
        return 2;
    }
    if (selection.parameters->genesis_block.header.time >
        std::numeric_limits<std::uint32_t>::max() - 600U) {
        std::cerr << "selected network has an invalid genesis timestamp\n";
        return 1;
    }
    const auto start_time = selection.parameters->genesis_block.header.time + 600U;
    if (bootstrap_config.has_value()) {
        const auto bootstrap =
            nova::node::LoadStaticBootstrapConfig(*bootstrap_config, *selection.parameters);
        if (bootstrap.error != nova::node::BootstrapConfigError::kNone) {
            std::cerr << "invalid static bootstrap configuration\n";
            return 2;
        }
        for (const auto& endpoint : bootstrap.seeds) {
            const auto duplicate =
                std::any_of(peers.begin(), peers.end(), [&endpoint](const auto& item) {
                    return item.host == endpoint.host && item.port == endpoint.port;
                });
            if (!duplicate) {
                peers.push_back(endpoint);
            }
        }
    }
#ifdef _WIN32
    char* wallet_passphrase{};
    std::size_t wallet_passphrase_length{};
    if (_dupenv_s(&wallet_passphrase, &wallet_passphrase_length, "NOVACOIN_WALLET_PASSPHRASE") !=
            0 ||
        wallet_passphrase == nullptr || wallet_passphrase[0] == '\0') {
        std::cerr << "NOVACOIN_WALLET_PASSPHRASE must be supplied out of band\n";
        return 2;
    }
    const std::string_view passphrase_view{wallet_passphrase};
#else
    const char* const wallet_passphrase = std::getenv("NOVACOIN_WALLET_PASSPHRASE");
    if (wallet_passphrase == nullptr || wallet_passphrase[0] == '\0') {
        std::cerr << "NOVACOIN_WALLET_PASSPHRASE must be supplied out of band\n";
        return 2;
    }
    const std::string_view passphrase_view{wallet_passphrase};
#endif
    if (passphrase_view.size() > 4'096U) {
#ifdef _WIN32
        ReleaseSecretEnvironmentString(wallet_passphrase, wallet_passphrase_length);
#endif
        std::cerr << "wallet passphrase exceeds configured limit\n";
        return 2;
    }
    const std::vector<std::uint8_t> passphrase{passphrase_view.begin(), passphrase_view.end()};
#ifdef _WIN32
    ReleaseSecretEnvironmentString(wallet_passphrase, wallet_passphrase_length);
#endif
    auto node = nova::node::RegtestNode::Create({*name, *data_directory, *log_path, *p2p_port,
                                                 *rpc_port, start_time, passphrase, *network});
    auto peer_service = node == nullptr ? nullptr : nova::node::PeerService::Create(*node);
    if (node == nullptr || peer_service == nullptr ||
        peer_service->Listen({p2p_bind, *p2p_port}) != nova::net::TransportError::kNone) {
        std::cerr << nova::node::NetworkName(*network) << " node P2P initialization failed\n";
        return 1;
    }
    // RPC credentials are intentionally provided via the environment, not
    // arguments or logs. The fixed account name keeps the surface small.
    const auto rpc_password = EnvironmentValue("NOVACOIN_RPC_PASSWORD");
    if (!rpc_password.has_value() || rpc_password->empty()) {
        std::cerr << "NOVACOIN_RPC_PASSWORD must be supplied out of band\n";
        return 2;
    }
    std::vector<nova::rpc::RpcRestrictedPrincipal> restricted_principals;
    const auto explorer_rpc_password = EnvironmentValue("NOVACOIN_EXPLORER_RPC_PASSWORD");
    if (explorer_rpc_password.has_value() && !explorer_rpc_password->empty()) {
        restricted_principals.push_back(
            {"explorer", *explorer_rpc_password, nova::rpc::RpcRole::kExplorer});
    }
    std::optional<nova::primitives::Amount> faucet_payout;
    if (*network == nova::consensus::NetworkId::kTestnet) {
        const auto faucet_rpc_password = EnvironmentValue("NOVACOIN_FAUCET_RPC_PASSWORD");
        const auto faucet_maximum = EnvironmentValue("NOVACOIN_FAUCET_MAX_PAYOUT");
        std::int64_t maximum_faucet_payout{};
        const auto maximum_text =
            faucet_maximum.has_value() ? std::string_view{*faucet_maximum} : std::string_view{};
        const auto parsed =
            maximum_text.empty()
                ? std::from_chars_result{maximum_text.data(), std::errc::invalid_argument}
                : std::from_chars(maximum_text.data(), maximum_text.data() + maximum_text.size(),
                                  maximum_faucet_payout);
        if (faucet_rpc_password.has_value() && !faucet_rpc_password->empty() &&
            parsed.ec == std::errc{} && parsed.ptr == maximum_text.data() + maximum_text.size() &&
            maximum_faucet_payout > 0) {
            restricted_principals.push_back(
                {"faucet", *faucet_rpc_password, nova::rpc::RpcRole::kFaucet});
            faucet_payout = maximum_faucet_payout;
        } else if (faucet_rpc_password.has_value() || faucet_maximum.has_value()) {
            std::cerr << "invalid TESTNET faucet RPC configuration\n";
            return 2;
        }
    }
    const auto maximum_faucet_payout = faucet_payout.value_or(0);
    nova::rpc::RpcConfig rpc_config{
        "127.0.0.1", "novacoin", *rpc_password, {8'192U, 1'000'000U, 4'096U}, *network};
    rpc_config.restricted_principals = std::move(restricted_principals);
    rpc_config.maximum_faucet_payout = maximum_faucet_payout;
    auto rpc_service = nova::rpc::RpcService::Create(
        std::move(rpc_config),
        {node->chain_state(),
         node->utxos(),
         node->mempool(),
         peer_service->peer_manager(),
         node->wallet(),
         {node->height() + 1U, node->next_mining_time()},
         NowSeconds(),
         [&node]() { return node->PersistWalletForRpc(); },
         [&node, &peer_service]() -> std::optional<nova::crypto::Hash256> {
             const auto mined = node->MineBlock();
             if (mined.error != nova::node::RegtestNodeError::kNone ||
                 !mined.block_hash.has_value()) {
                 return std::nullopt;
             }
             const auto index = node->chain_state().GetBlockIndex(*mined.block_hash);
             if (!index.has_value() || !index->block.has_value()) {
                 return std::nullopt;
             }
             peer_service->RelayBlock(*index->block);
             return mined.block_hash;
         },
         [&node, &peer_service](
             const nova::wallet::Recipient& recipient) -> std::optional<nova::crypto::Hash256> {
             const std::array<nova::wallet::Recipient, 1U> recipients{recipient};
             const auto submitted = node->CreateAndBroadcast(recipients);
             if (submitted.error != nova::node::RegtestNodeError::kNone ||
                 !submitted.transaction.has_value()) {
                 return std::nullopt;
             }
             const auto transaction_id =
                 submitted.transaction->TxId(node->wallet().transaction_limits());
             if (!transaction_id.has_value()) {
                 return std::nullopt;
             }
             peer_service->RelayTransaction(*submitted.transaction);
             return transaction_id;
         },
         [&peer_service](const nova::net::TcpEndpoint& endpoint) {
             return peer_service->Dial(endpoint, NowSeconds()).has_value();
         },
         [&peer_service]() { peer_service->DisconnectPeers(); },
         [&node]() { return node->metrics(); }});
    auto rpc_server =
        rpc_service == nullptr
            ? nullptr
            : nova::rpc::LoopbackHttpServer::Create(
                  {"127.0.0.1", *rpc_port, 32U, 8'192U, 1'000'000U, 30U}, *rpc_service);
    if (rpc_server == nullptr) {
        std::cerr << nova::node::NetworkName(*network) << " node RPC initialization failed\n";
        return 1;
    }
    for (const auto& endpoint : peers) {
        if (!peer_service->Dial(endpoint, NowSeconds()).has_value()) {
            std::cerr << "failed to dial " << endpoint.host << ':' << endpoint.port << '\n';
            return 1;
        }
    }
    std::cout << "novacoind " << nova::node::NetworkName(*network) << " P2P/RPC ready "
              << node->config().name << '\n';
    while (shutdown_requested == 0) {
        static_cast<void>(peer_service->Pump(NowSeconds()));
        static_cast<void>(rpc_server->Pump(NowSeconds()));
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    rpc_server->Close();
    peer_service->Close();
    if (!node->PersistWalletForRpc()) {
        std::cerr << "wallet persistence failed during shutdown\n";
        return 1;
    }
    std::cout << "novacoind shutdown complete " << node->config().name << '\n';
    return 0;
}
