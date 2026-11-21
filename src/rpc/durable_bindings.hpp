#pragma once

#include "node/durable_session.hpp"
#include "rpc/rpc.hpp"

namespace nova::rpc
{

// Caller-owned actions perform durable publication and any authorized relay.
// The session, peers and captured action state must outlive the RPC service.
struct DurableRpcActions final {
    std::function<bool(const primitives::Transaction&)> submit_transaction;
    std::function<std::optional<wallet::WalletAddress>()> generate_address;
    std::function<std::optional<crypto::Hash256>()> mine_block;
    std::function<std::optional<crypto::Hash256>(const wallet::Recipient&)> send_to_address;
    std::function<bool(const net::TcpEndpoint&)> connect_peer;
    std::function<void()> disconnect_peers;
    std::function<observability::MetricsSnapshot()> read_metrics;
    std::function<bool()> state_available;
    std::function<std::optional<crypto::Hash256>()> uncertain_payment_id;
};

[[nodiscard]] inline std::unique_ptr<RpcService>
CreateDurableRpcService(RpcConfig config, const node::DurableSession& session,
                        net::PeerManager& peers, DurableRpcActions actions) noexcept
{
    if (config.network != session.network_params().id || !session.View())
        return nullptr;
    try {
        config.require_node_mutation_callbacks = true;
        RpcDependencies dependencies{{}, {}, {}, peers, {}, {}, 0U, {}, {}, {}, {}, {}, {}};
        dependencies.mine_regtest_block = std::move(actions.mine_block);
        dependencies.send_to_address = std::move(actions.send_to_address);
        dependencies.connect_peer = std::move(actions.connect_peer);
        dependencies.disconnect_peers = std::move(actions.disconnect_peers);
        dependencies.read_metrics = std::move(actions.read_metrics);
        dependencies.submit_transaction = std::move(actions.submit_transaction);
        dependencies.generate_address = std::move(actions.generate_address);
        dependencies.uncertain_payment_id = std::move(actions.uncertain_payment_id);
        dependencies.state_available = [&session, guard = std::move(actions.state_available)] {
            return (!guard || guard()) && session.View().has_value();
        };
        dependencies.verify_transaction = [&session](const primitives::Transaction& tx) {
            return session.VerifyTransaction(tx);
        };
        dependencies.read_wallet = [&session]() -> std::optional<RpcWalletSnapshot> {
            auto snapshot = session.WalletView();
            if (!snapshot)
                return std::nullopt;
            return RpcWalletSnapshot{snapshot->confirmed, snapshot->unconfirmed,
                                     std::move(snapshot->coins)};
        };
        dependencies.build_transaction = [&session](std::span<const wallet::Recipient> recipients) {
            return session.BuildTransaction(recipients);
        };
        dependencies.sign_transaction = [&session](const primitives::Transaction& tx) {
            return session.SignTransaction(tx);
        };
        dependencies.find_block = [&session](const crypto::Hash256& hash) {
            auto result = session.FindBlock(hash);
            return RpcBlockLookup{result.available, std::move(result.block)};
        };
        dependencies.find_mempool_entry = [&session](const crypto::Hash256& hash) {
            auto result = session.FindMempoolEntry(hash);
            return RpcMempoolLookup{result.available, std::move(result.entry)};
        };
        dependencies.read_coin = [&session](const chain::UTXOKey& key) {
            return session.ReadCoin(key);
        };
        dependencies.read_chain = [&session]() -> std::optional<RpcChainSnapshot> {
            const auto view = session.View();
            if (!view)
                return std::nullopt;
            return RpcChainSnapshot{view->height, view->tip, view->chain_work, view->mempool_size,
                                    view->mempool_bytes};
        };
        dependencies.read_active_blocks = [&session] { return session.ActiveBlocks(); };
        return RpcService::Create(std::move(config), std::move(dependencies));
    } catch (...) {
        return nullptr;
    }
}
} // namespace nova::rpc
