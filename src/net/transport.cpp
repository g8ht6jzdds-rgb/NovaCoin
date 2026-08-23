#include "net/transport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nova::net
{
namespace
{

#ifdef _WIN32
using NativeSocket = SOCKET;
using NativeSocketLength = int;
using NativeSocketIoSize = int;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
[[nodiscard]] int LastSocketError() noexcept
{
    return WSAGetLastError();
}
[[nodiscard]] bool WouldBlock(const int error) noexcept
{
    return error == WSAEWOULDBLOCK;
}
void CloseNativeSocket(const NativeSocket socket) noexcept
{
    if (socket != kInvalidSocket) {
        static_cast<void>(closesocket(socket));
    }
}
#else
using NativeSocket = int;
using NativeSocketLength = socklen_t;
using NativeSocketIoSize = std::size_t;
constexpr NativeSocket kInvalidSocket = -1;
[[nodiscard]] int LastSocketError() noexcept
{
    return errno;
}
[[nodiscard]] bool WouldBlock(const int error) noexcept
{
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
}
void CloseNativeSocket(const NativeSocket socket) noexcept
{
    if (socket != kInvalidSocket) {
        static_cast<void>(close(socket));
    }
}
#endif

class Socket final
{
  public:
    Socket() = default;
    explicit Socket(const NativeSocket socket) noexcept : socket_(socket) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : socket_(std::exchange(other.socket_, kInvalidSocket)) {}
    Socket& operator=(Socket&& other) noexcept
    {
        if (this != &other) {
            Reset();
            socket_ = std::exchange(other.socket_, kInvalidSocket);
        }
        return *this;
    }
    ~Socket()
    {
        Reset();
    }

    [[nodiscard]] NativeSocket get() const noexcept
    {
        return socket_;
    }
    [[nodiscard]] bool valid() const noexcept
    {
        return socket_ != kInvalidSocket;
    }
    void Reset() noexcept
    {
        CloseNativeSocket(socket_);
        socket_ = kInvalidSocket;
    }

  private:
    NativeSocket socket_{kInvalidSocket};
};

[[nodiscard]] bool SetNonBlocking(const NativeSocket socket) noexcept
{
#ifdef _WIN32
    u_long enabled = 1U;
    return ioctlsocket(socket, static_cast<long>(FIONBIO), &enabled) == 0;
#else
    const auto flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

[[nodiscard]] std::optional<Socket> ConnectSocket(const TcpEndpoint& endpoint) noexcept
{
    if (endpoint.host.empty() || endpoint.host.size() > 255U || endpoint.port == 0U) {
        return std::nullopt;
    }
    try {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* results = nullptr;
        const auto port = std::to_string(endpoint.port);
        if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results) != 0) {
            return std::nullopt;
        }
        std::optional<Socket> connected;
        for (auto* current = results; current != nullptr; current = current->ai_next) {
            if (!std::in_range<NativeSocketLength>(current->ai_addrlen)) {
                continue;
            }
            Socket socket{::socket(current->ai_family, current->ai_socktype, current->ai_protocol)};
            if (!socket.valid() || !SetNonBlocking(socket.get())) {
                continue;
            }
            const auto result = connect(socket.get(), current->ai_addr,
                                        static_cast<NativeSocketLength>(current->ai_addrlen));
            if (result == 0 || WouldBlock(LastSocketError())) {
                connected = std::move(socket);
                break;
            }
        }
        freeaddrinfo(results);
        return connected;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<Socket> ListenSocket(const TcpEndpoint& endpoint) noexcept
{
    if (endpoint.host.empty() || endpoint.host.size() > 255U || endpoint.port == 0U) {
        return std::nullopt;
    }
    try {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;
        addrinfo* results = nullptr;
        const auto port = std::to_string(endpoint.port);
        if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results) != 0) {
            return std::nullopt;
        }
        std::optional<Socket> listener;
        for (auto* current = results; current != nullptr; current = current->ai_next) {
            if (!std::in_range<NativeSocketLength>(current->ai_addrlen)) {
                continue;
            }
            Socket socket{::socket(current->ai_family, current->ai_socktype, current->ai_protocol)};
            if (!socket.valid()) {
                continue;
            }
            int reuse = 1;
            static_cast<void>(setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR,
                                         reinterpret_cast<const char*>(&reuse), sizeof(reuse)));
            if (bind(socket.get(), current->ai_addr,
                     static_cast<NativeSocketLength>(current->ai_addrlen)) != 0 ||
                listen(socket.get(), SOMAXCONN) != 0 || !SetNonBlocking(socket.get())) {
                continue;
            }
            listener = std::move(socket);
            break;
        }
        freeaddrinfo(results);
        return listener;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

struct TcpTransport::Impl final {
    struct PeerSocket final {
        Socket socket;
        std::vector<std::uint8_t> pending_send;
        std::size_t sent_offset{};
    };

    explicit Impl(TcpTransportParams input) noexcept
        : parameters(std::move(input)), peers(parameters.peers)
    {
    }

    TcpTransportParams parameters;
    PeerManager peers;
    Socket listener;
    std::map<std::uint64_t, PeerSocket> sockets;
    std::set<std::uint64_t> close_requested;
};

TcpTransport::TcpTransport(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation))
{
}
TcpTransport::~TcpTransport()
{
    Close();
#ifdef _WIN32
    static_cast<void>(WSACleanup());
#endif
}

std::unique_ptr<TcpTransport> TcpTransport::Create(TcpTransportParams parameters) noexcept
{
    if (parameters.read_buffer_size == 0U || parameters.read_buffer_size > 1U * 1024U * 1024U ||
        parameters.max_socket_send_buffer == 0U ||
        parameters.max_socket_send_buffer > 16U * 1024U * 1024U ||
        parameters.maximum_accepted_per_poll == 0U) {
        return nullptr;
    }
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return nullptr;
    }
#endif
    try {
        return std::unique_ptr<TcpTransport>{
            new TcpTransport{std::make_unique<Impl>(std::move(parameters))}};
    } catch (...) {
#ifdef _WIN32
        static_cast<void>(WSACleanup());
#endif
        return nullptr;
    }
}

TransportError TcpTransport::Listen(const TcpEndpoint& endpoint) noexcept
{
    if (implementation_ == nullptr || implementation_->listener.valid()) {
        return TransportError::kInvalidParameters;
    }
    auto listener = ListenSocket(endpoint);
    if (!listener.has_value()) {
        return TransportError::kListenFailure;
    }
    implementation_->listener = std::move(*listener);
    return TransportError::kNone;
}

std::optional<std::uint64_t> TcpTransport::Dial(const TcpEndpoint& endpoint,
                                                const std::uint64_t now) noexcept
{
    if (implementation_ == nullptr) {
        return std::nullopt;
    }
    auto socket = ConnectSocket(endpoint);
    if (!socket.has_value()) {
        return std::nullopt;
    }
    const auto peer = implementation_->peers.AddOutbound(now);
    if (!peer.has_value()) {
        return std::nullopt;
    }
    try {
        implementation_->sockets.emplace(*peer, Impl::PeerSocket{std::move(*socket), {}, 0U});
        return peer;
    } catch (...) {
        return std::nullopt;
    }
}

TransportError TcpTransport::Queue(const std::uint64_t peer_id,
                                   const FramedMessage& message) noexcept
{
    if (implementation_ == nullptr || !implementation_->sockets.contains(peer_id)) {
        return TransportError::kSocketFailure;
    }
    return implementation_->peers.Queue(peer_id, message) == P2PError::kNone
               ? TransportError::kNone
               : TransportError::kQueueLimit;
}

void TcpTransport::Broadcast(const FramedMessage& message,
                             const std::optional<std::uint64_t> exclude_peer) noexcept
{
    if (implementation_ == nullptr) {
        return;
    }
    for (const auto& [peer_id, socket] : implementation_->sockets) {
        static_cast<void>(socket);
        if (!exclude_peer.has_value() || peer_id != *exclude_peer) {
            static_cast<void>(implementation_->peers.Queue(peer_id, message));
        }
    }
}

TransportResult TcpTransport::Pump(const std::uint64_t now, const MessageHandler& handler) noexcept
{
    if (implementation_ == nullptr || !handler) {
        return {TransportError::kInvalidParameters, 0U, 0U};
    }
    try {
        std::size_t disconnected{};
        for (std::uint32_t count = 0U;
             implementation_->listener.valid() &&
             count < implementation_->parameters.maximum_accepted_per_poll;
             ++count) {
            Socket accepted{accept(implementation_->listener.get(), nullptr, nullptr)};
            if (!accepted.valid()) {
                if (WouldBlock(LastSocketError()))
                    break;
                return {TransportError::kSocketFailure, 0U, disconnected};
            }
            if (!SetNonBlocking(accepted.get())) {
                continue;
            }
            const auto peer = implementation_->peers.AddInbound(now);
            if (!peer.has_value()) {
                continue;
            }
            implementation_->sockets.emplace(*peer, Impl::PeerSocket{std::move(accepted), {}, 0U});
        }

        std::size_t dispatched{};
        std::vector<std::uint8_t> read_buffer(implementation_->parameters.read_buffer_size);
        for (auto iterator = implementation_->sockets.begin();
             iterator != implementation_->sockets.end();) {
            const auto peer_id = iterator->first;
            auto& socket = iterator->second;
            bool remove = false;
            const auto received =
                recv(socket.socket.get(), reinterpret_cast<char*>(read_buffer.data()),
                     static_cast<NativeSocketIoSize>(read_buffer.size()), 0);
            if (received == 0) {
                remove = true;
            } else if (received < 0) {
                remove = !WouldBlock(LastSocketError());
            } else {
                const auto result =
                    implementation_->peers.Receive(peer_id,
                                                   std::span<const std::uint8_t>{read_buffer}.first(
                                                       static_cast<std::size_t>(received)),
                                                   now);
                if (result.error != P2PError::kNone) {
                    remove = true;
                } else {
                    for (const auto& event : result.events) {
                        handler(event.peer_id, event.message);
                        ++dispatched;
                    }
                    remove = implementation_->close_requested.contains(peer_id);
                }
            }
            for (auto& frame : implementation_->peers.TakeOutbound(peer_id)) {
                if (frame.size() > implementation_->parameters.max_socket_send_buffer ||
                    socket.pending_send.size() >
                        implementation_->parameters.max_socket_send_buffer - frame.size()) {
                    remove = true;
                    break;
                }
                socket.pending_send.insert(socket.pending_send.end(), frame.begin(), frame.end());
            }
            if (!remove && socket.sent_offset < socket.pending_send.size()) {
                const auto remaining = socket.pending_send.size() - socket.sent_offset;
                const auto written = send(
                    socket.socket.get(),
                    reinterpret_cast<const char*>(socket.pending_send.data() + socket.sent_offset),
                    static_cast<NativeSocketIoSize>(remaining), 0);
                if (written > 0) {
                    socket.sent_offset += static_cast<std::size_t>(written);
                    if (socket.sent_offset == socket.pending_send.size()) {
                        socket.pending_send.clear();
                        socket.sent_offset = 0U;
                    }
                } else if (written < 0 && !WouldBlock(LastSocketError())) {
                    remove = true;
                }
            }
            if (remove) {
                implementation_->close_requested.erase(peer_id);
                implementation_->peers.Disconnect(peer_id);
                iterator = implementation_->sockets.erase(iterator);
                ++disconnected;
            } else {
                ++iterator;
            }
        }
        implementation_->peers.Tick(now);
        return {TransportError::kNone, dispatched, disconnected};
    } catch (...) {
        return {TransportError::kAllocationFailure, 0U, 0U};
    }
}

void TcpTransport::ClosePeer(const std::uint64_t peer_id) noexcept
{
    if (implementation_ != nullptr) {
        try {
            implementation_->close_requested.insert(peer_id);
        } catch (...) {
            // If resource exhaustion prevents deferred close bookkeeping,
            // immediate close is still safe outside a callback.
        }
    }
}

void TcpTransport::DisconnectAllPeers() noexcept
{
    if (implementation_ == nullptr) {
        return;
    }
    for (const auto& [peer_id, socket] : implementation_->sockets) {
        static_cast<void>(socket);
        implementation_->peers.Disconnect(peer_id);
    }
    implementation_->sockets.clear();
    implementation_->close_requested.clear();
}

void TcpTransport::Close() noexcept
{
    if (implementation_ != nullptr) {
        DisconnectAllPeers();
        implementation_->listener.Reset();
    }
}

std::size_t TcpTransport::peer_count() const noexcept
{
    return implementation_ == nullptr ? 0U : implementation_->sockets.size();
}

PeerManager& TcpTransport::peer_manager() noexcept
{
    return implementation_->peers;
}

} // namespace nova::net
