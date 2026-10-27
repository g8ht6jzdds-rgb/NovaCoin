#include "chain/network_chainstate.hpp"
#include "rpc/rpc.hpp"

#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > 4096U)
        return 0;
    const auto& network = nova::consensus::RegtestNetworkParams();
    auto state = nova::chain::NetworkChainInitializer::Create(
        network.id, std::make_shared<nova::chain::FixedValidationTimeSource>(1704160000U));
    auto pool =
        nova::chain::Mempool::Create({nova::chain::NetworkValidationParams(network), 8U, 4096U, 0});
    auto wallet = nova::wallet::Wallet::Create({network.block_limits.transaction_limits, 1, 1U});
    const nova::net::P2PParams protocol{network.network_magic,
                                        network.minimum_peer_protocol_version,
                                        4096U,
                                        256U,
                                        8U,
                                        8U,
                                        8U,
                                        8U,
                                        network.block_limits.transaction_limits,
                                        network.block_limits};
    const nova::net::ConnectionParams connection{
        protocol, {network.protocol_version, 0U, 1, 1U, "/fuzz/", 0, true}, 10U, 20U, 10U, 8U, 8U,
        4096U};
    nova::net::PeerManager peers{{connection, 2U, 2U}};
    if (!state || !pool || !wallet)
        std::abort();
    nova::rpc::RpcConfig config{"127.0.0.1", "unused", "test-only", {1024U, 4096U, 1U}, network.id};
    config.restricted_principals.push_back({"reader", "test", nova::rpc::RpcRole::kExplorer});
    auto service = nova::rpc::RpcService::Create(config, {*state->chain,
                                                          *state->utxos,
                                                          *pool,
                                                          peers,
                                                          *wallet,
                                                          {1U, 1704160000U},
                                                          1704160000U,
                                                          {},
                                                          {},
                                                          {},
                                                          {},
                                                          {},
                                                          {}});
    if (!service)
        std::abort();
    const auto tip = state->chain->active_tip();
    const std::string body =
        size == 0U ? std::string{} : std::string(reinterpret_cast<const char*>(data), size);
    // Public test credentials only. No listener, private keys, persistence or mutation callbacks.
    static_cast<void>(service->HandleHttpPost({"POST", "Basic cmVhZGVyOnRlc3Q=", body}));
    if (state->chain->active_tip() != tip || !wallet->ListReceivingAddresses().empty())
        std::abort();
    return 0;
}
