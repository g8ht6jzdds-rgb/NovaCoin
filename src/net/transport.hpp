#pragma once

#include "net/p2p.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace nova::net
{

struct TcpEndpoint final {
    std::string host;
    std::uint16_t port{};
};

struct TcpTransportParams final {
    PeerManagerParams peers;
    std::size_t read_buffer_size{};
    std::size_t max_socket_send_buffer{};
    std::uint32_t maximum_accepted_per_poll{};
};

enum class TransportError : std::uint8_t {
    kNone,
    kInvalidParameters,
    kSocketFailure,
    kBindFailure,
    kListenFailure,
    kConnectFailure,
    kPeerLimit,
    kQueueLimit,
    kAllocationFailure,
};

struct TransportResult final {
    TransportError error{TransportError::kNone};
    std::size_t messages_dispatched{};
    std::size_t disconnected{};
};

// Owns all native sockets through RAII. It only forwards canonical messages
// emitted by PeerManager; consensus state remains in the caller's handler.
class TcpTransport final
{
  public:
    using MessageHandler = std::function<void(std::uint64_t, const FramedMessage&)>;

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;
    TcpTransport(TcpTransport&&) = delete;
    TcpTransport& operator=(TcpTransport&&) = delete;
    ~TcpTransport();

    [[nodiscard]] static std::unique_ptr<TcpTransport>
    Create(TcpTransportParams parameters) noexcept;
    [[nodiscard]] TransportError Listen(const TcpEndpoint& endpoint) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> Dial(const TcpEndpoint& endpoint,
                                                    std::uint64_t now) noexcept;
    [[nodiscard]] TransportError Queue(std::uint64_t peer_id,
                                       const FramedMessage& message) noexcept;
    void Broadcast(const FramedMessage& message,
                   std::optional<std::uint64_t> exclude_peer = std::nullopt) noexcept;
    [[nodiscard]] TransportResult Pump(std::uint64_t now, const MessageHandler& handler) noexcept;
    void ClosePeer(std::uint64_t peer_id) noexcept;
    // Drops active peers while preserving the listening socket for controlled
    // regtest partition/reconnect scenarios.
    void DisconnectAllPeers() noexcept;
    void Close() noexcept;
    [[nodiscard]] std::size_t peer_count() const noexcept;
    [[nodiscard]] PeerManager& peer_manager() noexcept;

  private:
    struct Impl;
    explicit TcpTransport(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace nova::net
