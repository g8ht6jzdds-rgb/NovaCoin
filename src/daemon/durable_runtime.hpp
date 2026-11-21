#pragma once

#include "node/durable_node.hpp"
#include "rpc/durable_bindings.hpp"

#include <chrono>

namespace nova::daemon
{
// RPC adapter. DurableNode owns publication, staging, peers and storage leases.
class DurableRuntime final
{
  public:
    [[nodiscard]] static std::unique_ptr<DurableRuntime>
    Create(std::unique_ptr<node::DurableSession> session,
           std::unique_ptr<node::StagingWorkspace> workspace,
           std::span<const std::uint8_t> passphrase, rpc::RpcConfig config,
           std::unique_ptr<storage::GenerationStore> directory_lease = nullptr) noexcept
    {
        if (!session || config.network != session->network_params().id)
            return nullptr;
        try {
            auto owner = node::DurableNode::Create(std::move(session), std::move(workspace),
                                                   passphrase, std::move(directory_lease));
            if (!owner)
                return nullptr;
            auto runtime = std::unique_ptr<DurableRuntime>{new DurableRuntime{std::move(owner)}};
            auto* self = runtime.get();
            rpc::DurableRpcActions actions;
            actions.read_metrics = [self] { return self->node_->Metrics(); };
            actions.state_available = [self] { return self->Ready(); };
            actions.generate_address = [self] { return self->node_->GenerateAddress(); };
            actions.submit_transaction = [self](const primitives::Transaction& tx) {
                return self->node_->Submit(tx);
            };
            actions.mine_block = [self] { return self->node_->Mine(); };
            actions.send_to_address = [self](const wallet::Recipient& recipient) {
                return self->node_->Send(recipient);
            };
            actions.uncertain_payment_id = [self] { return self->node_->UncertainPayment(); };
            actions.connect_peer = [self](const net::TcpEndpoint& endpoint) {
                const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                return self->Ready() &&
                       self->Peer()
                           .Dial(endpoint, now <= 0 ? 0U : static_cast<std::uint64_t>(now))
                           .has_value();
            };
            actions.disconnect_peers = [self] { self->Peer().DisconnectPeers(); };
            runtime->rpc_ =
                rpc::CreateDurableRpcService(std::move(config), runtime->node_->Session(),
                                             runtime->Peer().peer_manager(), std::move(actions));
            return runtime->rpc_ ? std::move(runtime) : nullptr;
        } catch (...) {
            return nullptr;
        }
    }
    [[nodiscard]] bool Ready() const noexcept
    {
        return node_->Ready();
    }
    [[nodiscard]] bool Shutdown() noexcept
    {
        return node_->Shutdown();
    }
    [[nodiscard]] net::TransportResult Pump(std::uint64_t now) noexcept
    {
        return node_->Pump(now);
    }
    [[nodiscard]] std::optional<node::DurableSessionView> View() const noexcept
    {
        return node_->View();
    }
    [[nodiscard]] node::PeerService& Peer() noexcept
    {
        return node_->Peer();
    }
    [[nodiscard]] rpc::RpcService& Rpc() noexcept
    {
        return *rpc_;
    }

  private:
    explicit DurableRuntime(std::unique_ptr<node::DurableNode> node) noexcept
        : node_(std::move(node))
    {
    }
    std::unique_ptr<node::DurableNode> node_;
    std::unique_ptr<rpc::RpcService> rpc_;
};
} // namespace nova::daemon
