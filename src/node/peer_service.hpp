#pragma once

#include "net/transport.hpp"
#include "node/body_request_recovery.hpp"
#include "node/peer_discovery.hpp"
#include "node/regtest_node.hpp"
#include "node/sync.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace nova::node
{

// Runtime adapter: P2P bytes are parsed by TcpTransport, headers are handled
// by Synchronizer, and full blocks/transactions enter RegtestNode's identical
// local/remote validation and journal pipeline.
class PeerService final : private BodyRequestRecovery::Sink
{
  public:
    PeerService(const PeerService&) = delete;
    PeerService& operator=(const PeerService&) = delete;

    [[nodiscard]] static std::unique_ptr<PeerService> Create(RegtestNode& node) noexcept;
    [[nodiscard]] net::TransportError Listen(const net::TcpEndpoint& endpoint) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> Dial(const net::TcpEndpoint& endpoint,
                                                    std::uint64_t now) noexcept;
    [[nodiscard]] net::TransportResult Pump(std::uint64_t now) noexcept;
    [[nodiscard]] std::size_t peer_count() const noexcept;
    [[nodiscard]] net::PeerManager& peer_manager() noexcept;
    [[nodiscard]] bool EnableDiscovery(std::uint16_t listening_port) noexcept;
    [[nodiscard]] const BodyRequestRecovery::Counters& recovery_counters() const noexcept;
    // Local node actions are explicitly relayed through the same framed P2P
    // transport used for remote messages; this never bypasses node validation.
    void RelayBlock(const primitives::Block& block) noexcept;
    void RelayTransaction(const primitives::Transaction& transaction) noexcept;
    void DisconnectPeers() noexcept;
    void Close() noexcept;

  private:
    friend class PeerServiceTestAccess;
    [[nodiscard]] bool Missing(const crypto::Hash256& hash) const noexcept override;
    [[nodiscard]] net::TransportError
    QueueBody(std::uint64_t peer, const net::FramedMessage& message) noexcept override;
    PeerService(RegtestNode& node, std::unique_ptr<net::TcpTransport> transport,
                Synchronizer synchronizer) noexcept;
    void Handle(std::uint64_t peer_id, const net::FramedMessage& message) noexcept;
    void QueueRequests(std::uint64_t peer_id, const SyncResult& result) noexcept;

    RegtestNode& node_;
    std::unique_ptr<net::TcpTransport> transport_;
    Synchronizer synchronizer_;
    std::unique_ptr<PeerDiscovery> discovery_;
    BodyRequestRecovery recovery_;
    // Private test seam; no daemon configuration, RPC or wire extension.
    using QueueHook = net::TransportError (*)(void*, std::uint64_t,
                                              const net::FramedMessage&) noexcept;
    QueueHook queue_hook_{};
    void* queue_context_{};
    const std::uint64_t* clock_override_{};
    std::uint64_t recovery_now_{};
};

} // namespace nova::node
