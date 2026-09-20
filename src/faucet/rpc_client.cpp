#include "faucet/rpc_client.hpp"

#include "wallet/address.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
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
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace nova::faucet
{
namespace
{

#ifdef _WIN32
using NativeSocket = SOCKET;
using NativeSocketIoSize = int;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(const NativeSocket socket) noexcept
{
    if (socket != kInvalidSocket) {
        static_cast<void>(closesocket(socket));
    }
}
class WinsockScope final
{
  public:
    [[nodiscard]] bool Start() noexcept
    {
        WSADATA data{};
        started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        return started_;
    }
    ~WinsockScope()
    {
        if (started_) {
            WSACleanup();
        }
    }

  private:
    bool started_{};
};
#else
using NativeSocket = int;
using NativeSocketIoSize = std::size_t;
constexpr NativeSocket kInvalidSocket = -1;
void CloseSocket(const NativeSocket socket) noexcept
{
    if (socket != kInvalidSocket) {
        static_cast<void>(close(socket));
    }
}
#endif

class Socket final
{
  public:
    explicit Socket(const NativeSocket socket = kInvalidSocket) noexcept : socket_(socket) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket()
    {
        CloseSocket(socket_);
    }
    [[nodiscard]] NativeSocket get() const noexcept
    {
        return socket_;
    }
    [[nodiscard]] bool valid() const noexcept
    {
        return socket_ != kInvalidSocket;
    }

  private:
    NativeSocket socket_;
};

[[nodiscard]] bool IsLoopback(const std::string_view host) noexcept
{
    return host == "127.0.0.1" || host == "::1";
}

[[nodiscard]] bool SetTimeouts(const NativeSocket socket) noexcept
{
#ifdef _WIN32
    constexpr DWORD kTimeoutMilliseconds = 5'000U;
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char*>(&kTimeoutMilliseconds),
                      sizeof(kTimeoutMilliseconds)) == 0 &&
           setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                      reinterpret_cast<const char*>(&kTimeoutMilliseconds),
                      sizeof(kTimeoutMilliseconds)) == 0;
#else
    constexpr timeval kTimeout{5, 0};
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &kTimeout, sizeof(kTimeout)) == 0 &&
           setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &kTimeout, sizeof(kTimeout)) == 0;
#endif
}

[[nodiscard]] std::string Base64(const std::string_view input)
{
    constexpr std::string_view kAlphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    std::string result;
    result.reserve(((input.size() + 2U) / 3U) * 4U);
    for (std::size_t offset = 0U; offset < input.size(); offset += 3U) {
        const auto first = static_cast<std::uint8_t>(input[offset]);
        const auto second =
            offset + 1U < input.size() ? static_cast<std::uint8_t>(input[offset + 1U]) : 0U;
        const auto third =
            offset + 2U < input.size() ? static_cast<std::uint8_t>(input[offset + 2U]) : 0U;
        result.push_back(kAlphabet[first >> 2U]);
        result.push_back(kAlphabet[((first & 0x03U) << 4U) | (second >> 4U)]);
        result.push_back(
            offset + 1U < input.size() ? kAlphabet[((second & 0x0FU) << 2U) | (third >> 6U)] : '=');
        result.push_back(offset + 2U < input.size() ? kAlphabet[third & 0x3FU] : '=');
    }
    return result;
}

[[nodiscard]] std::optional<std::size_t> ContentLength(const std::string_view headers) noexcept
{
    constexpr std::string_view kName{"content-length:"};
    std::optional<std::size_t> result;
    std::size_t position{};
    while (position < headers.size()) {
        const auto line_end = headers.find("\r\n", position);
        const auto end = line_end == std::string_view::npos ? headers.size() : line_end;
        const auto line = headers.substr(position, end - position);
        std::string lower;
        lower.reserve(line.size());
        for (const auto character : line) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        }
        if (lower.starts_with(kName)) {
            if (result.has_value()) {
                return std::nullopt;
            }
            auto value = std::string_view{lower}.substr(kName.size());
            while (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1U);
            }
            if (value.empty()) {
                return std::nullopt;
            }
            std::size_t parsed{};
            for (const auto character : value) {
                if (character < '0' || character > '9' ||
                    parsed > ((std::numeric_limits<std::size_t>::max)() - 9U) / 10U) {
                    return std::nullopt;
                }
                parsed = parsed * 10U + static_cast<std::size_t>(character - '0');
            }
            result = parsed;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        position = line_end + 2U;
    }
    return result;
}

[[nodiscard]] std::optional<std::string> Post(const FaucetRpcClientConfig& configuration,
                                              const std::string_view body) noexcept
{
#ifdef _WIN32
    WinsockScope winsock;
    if (!winsock.Start()) {
        return std::nullopt;
    }
#endif
    const bool ipv6 = configuration.host == "::1";
    Socket socket{::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (!socket.valid() || !SetTimeouts(socket.get())) {
        return std::nullopt;
    }
    if (ipv6) {
        sockaddr_in6 endpoint{};
        endpoint.sin6_family = AF_INET6;
        endpoint.sin6_port = htons(configuration.port);
        if (inet_pton(AF_INET6, "::1", &endpoint.sin6_addr) != 1 ||
            connect(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) !=
                0) {
            return std::nullopt;
        }
    } else {
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(configuration.port);
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) !=
            0) {
            return std::nullopt;
        }
    }
    const auto authorization = Base64(configuration.username + ":" + configuration.password);
    const std::string request{
        "POST /rpc HTTP/1.1\r\nHost: " + configuration.host + "\r\nAuthorization: Basic " +
        authorization + "\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + std::string{body}};
    std::size_t sent{};
    while (sent < request.size()) {
        const auto result = send(socket.get(), request.data() + sent,
                                 static_cast<NativeSocketIoSize>(request.size() - sent), 0);
        if (result <= 0) {
            return std::nullopt;
        }
        sent += static_cast<std::size_t>(result);
    }
    std::string response;
    response.reserve(std::min(configuration.maximum_response_bytes, std::size_t{8U * 1024U}));
    std::array<char, 4096U> buffer{};
    while (true) {
        const auto received =
            recv(socket.get(), buffer.data(), static_cast<NativeSocketIoSize>(buffer.size()), 0);
        if (received == 0) {
            break;
        }
        if (received < 0 || response.size() > configuration.maximum_response_bytes ||
            static_cast<std::size_t>(received) >
                configuration.maximum_response_bytes - response.size()) {
            return std::nullopt;
        }
        response.append(buffer.data(), static_cast<std::size_t>(received));
    }
    const auto headers_end = response.find("\r\n\r\n");
    if (headers_end == std::string::npos || !response.starts_with("HTTP/1.1 200 ")) {
        return std::nullopt;
    }
    const auto length = ContentLength(std::string_view{response}.substr(0U, headers_end));
    const auto body_offset = headers_end + 4U;
    if (!length.has_value() || *length > configuration.maximum_response_bytes ||
        *length != response.size() - body_offset) {
        return std::nullopt;
    }
    return response.substr(body_offset);
}

[[nodiscard]] std::optional<crypto::Hash256> TransactionId(const std::string_view response) noexcept
{
    constexpr std::string_view kPrefix{"{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\""};
    if (!response.starts_with(kPrefix) || !response.ends_with("\"}")) {
        return std::nullopt;
    }
    const auto encoded = response.substr(kPrefix.size(), response.size() - kPrefix.size() - 2U);
    return crypto::Hash256::FromHex(encoded);
}

} // namespace

FaucetRpcClient::FaucetRpcClient(FaucetRpcClientConfig configuration) noexcept
    : configuration_(std::move(configuration))
{
}

std::unique_ptr<FaucetRpcClient>
FaucetRpcClient::Create(FaucetRpcClientConfig configuration) noexcept
{
    if (!IsLoopback(configuration.host) || configuration.port == 0U ||
        configuration.username.empty() || configuration.password.empty() ||
        configuration.maximum_response_bytes == 0U || configuration.network == nullptr ||
        configuration.network->id != consensus::NetworkId::kTestnet ||
        !configuration.network->enabled ||
        consensus::CheckNetworkParams(*configuration.network) !=
            consensus::NetworkParamsError::kNone) {
        return nullptr;
    }
    try {
        return std::unique_ptr<FaucetRpcClient>{new FaucetRpcClient{std::move(configuration)}};
    } catch (...) {
        return nullptr;
    }
}

std::optional<crypto::Hash256>
FaucetRpcClient::Pay(const wallet::Recipient& recipient) const noexcept
{
    if (recipient.amount <= 0) {
        return std::nullopt;
    }
    const auto address =
        wallet::EncodeP2pkhAddress(recipient.public_key_hash, *configuration_.network);
    if (!address.has_value()) {
        return std::nullopt;
    }
    const std::string request{
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"faucetpay\",\"params\":{\"address\":\"" +
        *address + "\",\"amount\":" + std::to_string(recipient.amount) + "}}"};
    const auto response = Post(configuration_, request);
    return response.has_value() ? TransactionId(*response) : std::nullopt;
}

} // namespace nova::faucet
