#pragma once

#include "net/transport.hpp"
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
class PeerService final
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
    // Local node actions are explicitly relayed through the same framed P2P
    // transport used for remote messages; this never bypasses node validation.
    void RelayBlock(const primitives::Block& block) noexcept;
    void RelayTransaction(const primitives::Transaction& transaction) noexcept;
    void DisconnectPeers() noexcept;
    void Close() noexcept;

  private:
    PeerService(RegtestNode& node, std::unique_ptr<net::TcpTransport> transport,
                Synchronizer synchronizer) noexcept;
    void Handle(std::uint64_t peer_id, const net::FramedMessage& message) noexcept;
    void QueueRequests(std::uint64_t peer_id, const SyncResult& result) noexcept;

    RegtestNode& node_;
    std::unique_ptr<net::TcpTransport> transport_;
    Synchronizer synchronizer_;
};

} // namespace nova::node
