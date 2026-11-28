#pragma once

#include "net/transport.hpp"
#include "node/body_request_recovery.hpp"
#include "node/durable_session.hpp"
#include "node/peer_discovery.hpp"
#include "node/regtest_node.hpp"
#include "node/sync.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace nova::node
{

// The provider supplies a fresh private path and a borrowed secret valid only
// for the synchronous operation. It does not perform admission itself.
struct DurablePeerWriteContext final {
    std::filesystem::path staging_directory;
    std::span<const std::uint8_t> passphrase;
    // Optional caller-owned cleanup. Failure makes the adapter unavailable.
    std::function<bool()> cleanup{};
};

// Runtime adapter: P2P bytes are parsed by TcpTransport, headers are handled
// by Synchronizer, and supported full-block/transaction writers enter the
// durable coordinator. The legacy overload is linked only by test support.
class PeerService final : private BodyRequestRecovery::Sink
{
  public:
    PeerService(const PeerService&) = delete;
    PeerService& operator=(const PeerService&) = delete;

    // Definition intentionally absent from production libraries. Historical
    // regression fixtures link nova_legacy_node_test_support explicitly.
    [[nodiscard]] static std::unique_ptr<PeerService> Create(RegtestNode& node) noexcept;
    [[nodiscard]] static std::unique_ptr<PeerService>
    Create(DurableSession& session,
           std::function<std::optional<DurablePeerWriteContext>()> write_context) noexcept;
    // The caller retains the session and callbacks; callbacks must enter that session's
    // durable admission methods and must not report success before publication.
    [[nodiscard]] static std::unique_ptr<PeerService>
    Create(DurableSession& session,
           std::function<DurableSessionResult(const primitives::Block&)> accept_block,
           std::function<DurableSessionError(const primitives::Transaction&)>
               accept_transaction) noexcept;
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
    // Prevent any subsequent durable-owner message handling or relay.
    void QuarantineDurable() noexcept;
    [[nodiscard]] bool DurableAvailable() const noexcept
    {
        return !durable_failed_;
    }
    void DisconnectPeers() noexcept;
    void Close() noexcept;

  private:
    friend class PeerServiceTestAccess;
    [[nodiscard]] BodyRequestRecovery::BodyState
    State(const crypto::Hash256& hash) const noexcept override;
    [[nodiscard]] net::TransportError
    QueueBody(std::uint64_t peer, const net::FramedMessage& message) noexcept override;
    PeerService(RegtestNode& node, std::unique_ptr<net::TcpTransport> transport,
                Synchronizer synchronizer);
    PeerService(DurableSession& session, std::unique_ptr<net::TcpTransport> transport,
                std::function<DurableSessionResult(const primitives::Block&)> accept_block,
                std::function<DurableSessionError(const primitives::Transaction&)>
                    accept_transaction) noexcept;
    void Handle(std::uint64_t peer_id, const net::FramedMessage& message) noexcept;
    void QueueRequests(std::uint64_t peer_id, const SyncResult& result) noexcept;

    [[nodiscard]] static std::unique_ptr<net::TcpTransport>
    MakeLegacyTestTransport(const consensus::NetworkParams& network) noexcept;
    // Only the separately linked legacy test constructor populates this adapter.
    // Keeping its implementation outside nova_node removes independent legacy
    // wallet/journal writers from the supported production link graph.
    struct LegacyAdapter final {
        std::function<void(const net::TransportResult&)> record_transport;
        std::function<BodyRequestRecovery::BodyState(const crypto::Hash256&)> body_state;
        std::function<SyncResult()> start;
        std::function<SyncResult(const net::FramedMessage&)> headers;
        std::function<RegtestNodeError(const primitives::Block&)> block;
        std::function<std::optional<net::HeadersMessage>(const net::GetHeadersMessage&)>
            headers_for_locator;
        std::function<std::optional<chain::BlockIndex>(const crypto::Hash256&)> find_block;
        std::function<RegtestNodeError(const primitives::Transaction&)> transaction;
    };

    RegtestNode* node_{};
    const consensus::NetworkParams* network_{};
    std::unique_ptr<LegacyAdapter> legacy_;
    DurableSession* durable_{};
    bool durable_failed_{};
    std::function<DurableSessionResult(const primitives::Block&)> durable_block_;
    std::function<DurableSessionError(const primitives::Transaction&)> durable_transaction_;
    std::unique_ptr<net::TcpTransport> transport_;
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
