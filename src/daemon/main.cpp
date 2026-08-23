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
               "[--connect host:port]\n";
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
#ifdef _WIN32
    char* rpc_password_raw{};
    std::size_t rpc_password_length{};
    if (_dupenv_s(&rpc_password_raw, &rpc_password_length, "NOVACOIN_RPC_PASSWORD") != 0 ||
        rpc_password_raw == nullptr || rpc_password_raw[0] == '\0') {
        std::cerr << "NOVACOIN_RPC_PASSWORD must be supplied out of band\n";
        return 2;
    }
    const std::string rpc_password{rpc_password_raw};
    ReleaseSecretEnvironmentString(rpc_password_raw, rpc_password_length);
#else
    const char* const rpc_password_raw = std::getenv("NOVACOIN_RPC_PASSWORD");
    if (rpc_password_raw == nullptr || rpc_password_raw[0] == '\0') {
        std::cerr << "NOVACOIN_RPC_PASSWORD must be supplied out of band\n";
        return 2;
    }
    const std::string rpc_password{rpc_password_raw};
#endif
    auto rpc_service = nova::rpc::RpcService::Create(
        {"127.0.0.1", "novacoin", rpc_password, {8'192U, 1'000'000U, 4'096U}, *network},
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
