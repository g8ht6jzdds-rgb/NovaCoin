#include "faucet/http_server.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nova::faucet
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
    return error == EAGAIN || error == EWOULDBLOCK;
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

[[nodiscard]] bool IsLoopback(const std::string_view address) noexcept
{
    return address == "127.0.0.1" || address == "::1";
}

[[nodiscard]] std::string_view Trim(const std::string_view value) noexcept
{
    std::size_t first{};
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}

[[nodiscard]] std::string ToLower(const std::string_view input)
{
    std::string result;
    result.reserve(input.size());
    for (const auto character : input) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

[[nodiscard]] std::optional<std::size_t> ParseSize(const std::string_view input) noexcept
{
    if (input.empty()) {
        return std::nullopt;
    }
    std::size_t result{};
    for (const auto character : input) {
        if (character < '0' || character > '9' ||
            result > ((std::numeric_limits<std::size_t>::max)() -
                      static_cast<std::size_t>(character - '0')) /
                         10U) {
            return std::nullopt;
        }
        result = result * 10U + static_cast<std::size_t>(character - '0');
    }
    return result;
}

[[nodiscard]] std::string MakeResponse(const std::uint16_t status, const std::string_view body)
{
    const std::string_view reason = status == 200U   ? "OK"
                                    : status == 400U ? "Bad Request"
                                    : status == 405U ? "Method Not Allowed"
                                    : status == 429U ? "Too Many Requests"
                                    : status == 503U ? "Service Unavailable"
                                                     : "Internal Server Error";
    return "HTTP/1.1 " + std::to_string(status) + " " + std::string{reason} +
           "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + std::string{body};
}

[[nodiscard]] std::string InvalidRequestResponse()
{
    return MakeResponse(400U, "{\"error\":\"invalid_http_request\"}");
}

struct ParsedRequest final {
    std::string_view method;
    std::string_view target;
    std::string_view body;
};

enum class ParseState : std::uint8_t { kIncomplete, kReady, kInvalid };

[[nodiscard]] ParseState ParseRequest(const std::string& input,
                                      const FaucetHttpServerParams& parameters,
                                      ParsedRequest& result) noexcept
{
    const auto header_end = input.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return input.size() > parameters.max_header_bytes ? ParseState::kInvalid
                                                          : ParseState::kIncomplete;
    }
    if (header_end + 4U > parameters.max_header_bytes) {
        return ParseState::kInvalid;
    }
    const std::string_view header{input.data(), header_end};
    const auto first_end = header.find("\r\n");
    if (first_end == std::string_view::npos) {
        return ParseState::kInvalid;
    }
    const auto first_line = header.substr(0U, first_end);
    const auto first_space = first_line.find(' ');
    const auto second_space = first_space == std::string_view::npos
                                  ? std::string_view::npos
                                  : first_line.find(' ', first_space + 1U);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
        first_line.find(' ', second_space + 1U) != std::string_view::npos ||
        first_line.substr(second_space + 1U) != "HTTP/1.1") {
        return ParseState::kInvalid;
    }
    const auto method = first_line.substr(0U, first_space);
    const auto target = first_line.substr(first_space + 1U, second_space - first_space - 1U);
    if (method != "POST" || target != "/api/v1/request") {
        return ParseState::kInvalid;
    }
    std::optional<std::size_t> content_length;
    std::size_t position = first_end + 2U;
    while (position < header.size()) {
        const auto line_end = header.find("\r\n", position);
        const auto end = line_end == std::string_view::npos ? header.size() : line_end;
        const auto line = header.substr(position, end - position);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            return ParseState::kInvalid;
        }
        const auto name = ToLower(line.substr(0U, colon));
        const auto value = Trim(line.substr(colon + 1U));
        if (name == "content-length") {
            if (content_length.has_value()) {
                return ParseState::kInvalid;
            }
            content_length = ParseSize(value);
        } else if (name == "transfer-encoding" || name == "expect") {
            return ParseState::kInvalid;
        }
        if (!content_length.has_value() && name == "content-length") {
            return ParseState::kInvalid;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        position = line_end + 2U;
    }
    if (!content_length.has_value() || *content_length == 0U ||
        *content_length > parameters.max_body_bytes) {
        return ParseState::kInvalid;
    }
    const auto body_start = header_end + 4U;
    if (*content_length > input.size() - body_start) {
        return ParseState::kIncomplete;
    }
    if (*content_length != input.size() - body_start) {
        return ParseState::kInvalid;
    }
    result = ParsedRequest{method, target, std::string_view{input}.substr(body_start)};
    return ParseState::kReady;
}

[[nodiscard]] std::string PeerIdentifier(const sockaddr_storage& address) noexcept
{
    std::array<char, INET6_ADDRSTRLEN> text{};
    const void* source{};
    int family{};
    if (address.ss_family == AF_INET) {
        source = &reinterpret_cast<const sockaddr_in*>(&address)->sin_addr;
        family = AF_INET;
    } else if (address.ss_family == AF_INET6) {
        source = &reinterpret_cast<const sockaddr_in6*>(&address)->sin6_addr;
        family = AF_INET6;
    } else {
        return {};
    }
    return inet_ntop(family, source, text.data(), static_cast<socklen_t>(text.size())) == nullptr
               ? std::string{}
               : std::string{text.data()};
}

} // namespace

struct FaucetLoopbackHttpServer::Impl final {
    struct Client final {
        Socket socket;
        std::string source_identifier;
        std::string input;
        std::string output;
        std::size_t sent{};
        std::uint64_t last_activity{};
    };

    FaucetHttpServerParams parameters;
    FaucetHttpService& service;
    Socket listener;
    std::map<NativeSocket, Client> clients;
    std::uint16_t port{};
#ifdef _WIN32
    bool winsock_started{};
#endif

    Impl(FaucetHttpServerParams server_parameters, FaucetHttpService& http_service) noexcept
        : parameters(std::move(server_parameters)), service(http_service)
    {
    }
    ~Impl()
    {
        clients.clear();
        listener.Reset();
#ifdef _WIN32
        if (winsock_started) {
            WSACleanup();
        }
#endif
    }
};

FaucetLoopbackHttpServer::FaucetLoopbackHttpServer(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation))
{
}

FaucetLoopbackHttpServer::~FaucetLoopbackHttpServer() = default;

std::unique_ptr<FaucetLoopbackHttpServer>
FaucetLoopbackHttpServer::Create(FaucetHttpServerParams parameters,
                                 FaucetHttpService& service) noexcept
{
    if (!IsLoopback(parameters.bind_address) || parameters.max_connections == 0U ||
        parameters.max_header_bytes == 0U || parameters.max_body_bytes == 0U ||
        parameters.max_response_bytes == 0U || parameters.idle_timeout_seconds == 0U) {
        return nullptr;
    }
    if (parameters.max_body_bytes >
        (std::numeric_limits<std::size_t>::max)() - parameters.max_header_bytes) {
        return nullptr;
    }
    try {
        auto implementation = std::make_unique<Impl>(std::move(parameters), service);
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return nullptr;
        }
        implementation->winsock_started = true;
#endif
        const bool ipv6 = implementation->parameters.bind_address == "::1";
        Socket listener{::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        if (!listener.valid()) {
            return nullptr;
        }
        int reuse = 1;
        if (setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0 ||
            !SetNonBlocking(listener.get())) {
            return nullptr;
        }
        if (ipv6) {
            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_port = htons(implementation->parameters.port);
            if (inet_pton(AF_INET6, "::1", &address.sin6_addr) != 1 ||
                bind(listener.get(), reinterpret_cast<const sockaddr*>(&address),
                     static_cast<NativeSocketLength>(sizeof(address))) != 0) {
                return nullptr;
            }
        } else {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(implementation->parameters.port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(listener.get(), reinterpret_cast<const sockaddr*>(&address),
                     static_cast<NativeSocketLength>(sizeof(address))) != 0) {
                return nullptr;
            }
        }
        if (listen(listener.get(), static_cast<int>(implementation->parameters.max_connections)) !=
            0) {
            return nullptr;
        }
        sockaddr_storage bound{};
        NativeSocketLength bound_size = static_cast<NativeSocketLength>(sizeof(bound));
        if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
            return nullptr;
        }
        implementation->port =
            bound.ss_family == AF_INET
                ? ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port)
                : ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
        implementation->listener = std::move(listener);
        return std::unique_ptr<FaucetLoopbackHttpServer>{
            new FaucetLoopbackHttpServer{std::move(implementation)}};
    } catch (...) {
        return nullptr;
    }
}

FaucetHttpServerResult FaucetLoopbackHttpServer::Pump(const std::uint64_t now) noexcept
{
    if (implementation_ == nullptr) {
        return {FaucetHttpServerError::kSocketFailure, 0U, 0U, 0U};
    }
    try {
        std::size_t accepted{};
        std::size_t completed{};
        std::size_t rejected{};
        while (accepted < implementation_->parameters.max_connections) {
            sockaddr_storage address{};
            NativeSocketLength size = static_cast<NativeSocketLength>(sizeof(address));
            const NativeSocket raw = accept(implementation_->listener.get(),
                                            reinterpret_cast<sockaddr*>(&address), &size);
            if (raw == kInvalidSocket) {
                if (!WouldBlock(LastSocketError())) {
                    return {FaucetHttpServerError::kSocketFailure, accepted, completed, rejected};
                }
                break;
            }
            Socket client{raw};
            const auto source = PeerIdentifier(address);
            if (!SetNonBlocking(client.get()) || source.empty() ||
                implementation_->clients.size() >= implementation_->parameters.max_connections) {
                ++rejected;
                continue;
            }
            const auto client_id = client.get();
            implementation_->clients.emplace(
                client_id, Impl::Client{std::move(client), source, {}, {}, 0U, now});
            ++accepted;
        }
        for (auto iterator = implementation_->clients.begin();
             iterator != implementation_->clients.end();) {
            auto& client = iterator->second;
            bool close{};
            if (now < client.last_activity ||
                now - client.last_activity > implementation_->parameters.idle_timeout_seconds) {
                close = true;
                ++rejected;
            }
            if (!close && client.output.empty()) {
                std::array<char, 4096U> buffer{};
                while (true) {
                    const auto received = recv(client.socket.get(), buffer.data(),
                                               static_cast<NativeSocketIoSize>(buffer.size()), 0);
                    if (received > 0) {
                        const auto count = static_cast<std::size_t>(received);
                        const auto maximum = implementation_->parameters.max_header_bytes +
                                             implementation_->parameters.max_body_bytes;
                        if (client.input.size() > maximum ||
                            count > maximum - client.input.size()) {
                            client.output = InvalidRequestResponse();
                            ++rejected;
                            break;
                        }
                        client.input.append(buffer.data(), count);
                        client.last_activity = now;
                        continue;
                    }
                    if (received == 0 || !WouldBlock(LastSocketError())) {
                        close = true;
                    }
                    break;
                }
                if (!close && client.output.empty()) {
                    ParsedRequest request{};
                    const auto parsed =
                        ParseRequest(client.input, implementation_->parameters, request);
                    if (parsed == ParseState::kInvalid) {
                        client.output = InvalidRequestResponse();
                        ++rejected;
                    } else if (parsed == ParseState::kReady) {
                        const auto response = implementation_->service.Handle(
                            {request.method, request.target, request.body, client.source_identifier,
                             now});
                        client.output =
                            response.body.size() > implementation_->parameters.max_response_bytes
                                ? MakeResponse(503U, "{\"error\":\"response_exceeds_limit\"}")
                                : MakeResponse(response.status, response.body);
                    }
                }
            }
            if (!close && !client.output.empty()) {
                const auto remaining = client.output.size() - client.sent;
                const auto sent = send(client.socket.get(), client.output.data() + client.sent,
                                       static_cast<NativeSocketIoSize>(remaining), 0);
                if (sent > 0) {
                    client.sent += static_cast<std::size_t>(sent);
                    client.last_activity = now;
                    if (client.sent == client.output.size()) {
                        close = true;
                        ++completed;
                    }
                } else if (sent < 0 && !WouldBlock(LastSocketError())) {
                    close = true;
                }
            }
            if (close) {
                iterator = implementation_->clients.erase(iterator);
            } else {
                ++iterator;
            }
        }
        return {FaucetHttpServerError::kNone, accepted, completed, rejected};
    } catch (...) {
        return {FaucetHttpServerError::kAllocationFailure, 0U, 0U, 0U};
    }
}

std::uint16_t FaucetLoopbackHttpServer::port() const noexcept
{
    return implementation_ == nullptr ? 0U : implementation_->port;
}

void FaucetLoopbackHttpServer::Close() noexcept
{
    if (implementation_ != nullptr) {
        implementation_->clients.clear();
        implementation_->listener.Reset();
    }
}

} // namespace nova::faucet
