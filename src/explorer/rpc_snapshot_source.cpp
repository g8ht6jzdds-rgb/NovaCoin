#include "explorer/explorer.hpp"

#include "primitives/serialization.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

namespace nova::explorer
{
namespace
{

constexpr std::uint32_t kSnapshotVersion = 1U;
constexpr std::uint64_t kMaximumSnapshotBlocks = 1'000'000U;

#ifdef _WIN32
using NativeSocket = SOCKET;
using NativeSocketLength = int;
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
using NativeSocketLength = socklen_t;
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

[[nodiscard]] std::optional<std::vector<std::uint8_t>> DecodeHex(const std::string_view encoded,
                                                                 const std::size_t maximum) noexcept
{
    if (encoded.empty() || (encoded.size() % 2U) != 0U || encoded.size() / 2U > maximum) {
        return std::nullopt;
    }
    try {
        std::vector<std::uint8_t> result;
        result.reserve(encoded.size() / 2U);
        const auto nibble = [](const char character) -> std::optional<std::uint8_t> {
            if (character >= '0' && character <= '9') {
                return static_cast<std::uint8_t>(character - '0');
            }
            if (character >= 'a' && character <= 'f') {
                return static_cast<std::uint8_t>(character - 'a' + 10);
            }
            return character >= 'A' && character <= 'F'
                       ? std::optional<std::uint8_t>{static_cast<std::uint8_t>(character - 'A' +
                                                                               10)}
                       : std::nullopt;
        };
        for (std::size_t index = 0U; index < encoded.size(); index += 2U) {
            const auto high = nibble(encoded[index]);
            const auto low = nibble(encoded[index + 1U]);
            if (!high.has_value() || !low.has_value()) {
                return std::nullopt;
            }
            result.push_back(static_cast<std::uint8_t>((*high << 4U) | *low));
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
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
            const auto value = std::string_view{lower}.substr(kName.size());
            std::size_t first{};
            while (first < value.size() && value[first] == ' ') {
                ++first;
            }
            if (first == value.size()) {
                return std::nullopt;
            }
            std::size_t parsed{};
            for (const auto character : value.substr(first)) {
                if (character < '0' || character > '9' ||
                    parsed > (std::numeric_limits<std::size_t>::max() - 9U) / 10U) {
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

[[nodiscard]] std::optional<std::string> RequestSnapshot(const RpcSnapshotConfig& config) noexcept
{
#ifdef _WIN32
    WinsockScope winsock;
    if (!winsock.Start()) {
        return std::nullopt;
    }
#endif
    const bool ipv6 = config.host == "::1";
    Socket socket{::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (!socket.valid() || !SetTimeouts(socket.get())) {
        return std::nullopt;
    }
    if (ipv6) {
        sockaddr_in6 endpoint{};
        endpoint.sin6_family = AF_INET6;
        endpoint.sin6_port = htons(config.port);
        if (inet_pton(AF_INET6, "::1", &endpoint.sin6_addr) != 1 ||
            connect(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint),
                    static_cast<NativeSocketLength>(sizeof(endpoint))) != 0) {
            return std::nullopt;
        }
    } else {
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(config.port);
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint),
                    static_cast<NativeSocketLength>(sizeof(endpoint))) != 0) {
            return std::nullopt;
        }
    }
    constexpr std::string_view kBody{
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getexplorersnapshot\"}"};
    const auto credentials = Base64(config.username + ":" + config.password);
    const std::string request{
        "POST /rpc HTTP/1.1\r\nHost: " + config.host + "\r\nAuthorization: Basic " + credentials +
        "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(kBody.size()) +
        "\r\nConnection: close\r\n\r\n" + std::string{kBody}};
    std::size_t sent{};
    while (sent < request.size()) {
        const auto result =
            send(socket.get(), request.data() + sent,
                 static_cast<NativeSocketIoSize>(request.size() - sent), 0);
        if (result <= 0) {
            return std::nullopt;
        }
        sent += static_cast<std::size_t>(result);
    }
    std::string response;
    response.reserve(std::min(config.max_response_bytes, std::size_t{8U * 1024U}));
    std::array<char, 4096U> buffer{};
    while (true) {
        const auto received =
            recv(socket.get(), buffer.data(), static_cast<NativeSocketIoSize>(buffer.size()), 0);
        if (received == 0) {
            break;
        }
        if (received < 0 || response.size() > config.max_response_bytes ||
            static_cast<std::size_t>(received) > config.max_response_bytes - response.size()) {
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
    if (!length.has_value() || *length > config.max_response_bytes ||
        *length != response.size() - body_offset) {
        return std::nullopt;
    }
    return response.substr(body_offset);
}

[[nodiscard]] std::optional<std::string_view> SnapshotHex(const std::string_view body) noexcept
{
    constexpr std::string_view kEncoding{"\"encoding\":\"nova-snapshot-v1\""};
    constexpr std::string_view kData{"\"data\":\""};
    if (!body.starts_with("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{") ||
        body.find(kEncoding) == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = body.find(kData);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const auto data_start = start + kData.size();
    const auto data_end = body.find('"', data_start);
    if (data_end == std::string_view::npos || body.substr(data_end) != "\"}}") {
        return std::nullopt;
    }
    return body.substr(data_start, data_end - data_start);
}

} // namespace

AuthenticatedRpcSnapshotSource::AuthenticatedRpcSnapshotSource(RpcSnapshotConfig config) noexcept
    : config_(std::move(config))
{
}

std::unique_ptr<AuthenticatedRpcSnapshotSource>
AuthenticatedRpcSnapshotSource::Create(RpcSnapshotConfig config) noexcept
{
    if (!IsLoopback(config.host) || config.port == 0U || config.username.empty() ||
        config.password.empty() || config.max_response_bytes == 0U || config.network == nullptr) {
        return nullptr;
    }
    try {
        return std::unique_ptr<AuthenticatedRpcSnapshotSource>{
            new AuthenticatedRpcSnapshotSource{std::move(config)}};
    } catch (...) {
        return nullptr;
    }
}

std::optional<ExplorerSnapshot> AuthenticatedRpcSnapshotSource::ReadSnapshot() const noexcept
{
    const auto response = RequestSnapshot(config_);
    const auto encoded = response.has_value() ? SnapshotHex(*response) : std::nullopt;
    const auto bytes =
        encoded.has_value() ? DecodeHex(*encoded, config_.max_response_bytes) : std::nullopt;
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    primitives::BinaryReader reader{*bytes};
    const auto version = reader.ReadU32();
    const auto count = reader.ReadCompactSize(kMaximumSnapshotBlocks);
    if (!version.has_value() || *version != kSnapshotVersion || !count.has_value()) {
        return std::nullopt;
    }
    // Every record contains at least its four-byte height and a one-byte
    // compact block length.  Reject impossible counts before reserving.
    if (*count > static_cast<std::uint64_t>(reader.remaining() / 5U) ||
        *count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    try {
        ExplorerSnapshot snapshot{config_.network->id,
                                  config_.network->difficulty.target_spacing_seconds,
                                  config_.network->pow,
                                  config_.network->block_limits,
                                  {}};
        snapshot.active_blocks.reserve(static_cast<std::size_t>(*count));
        for (std::uint64_t index = 0U; index < *count; ++index) {
            const auto height = reader.ReadU32();
            const auto encoded_block =
                reader.ReadBytes(config_.network->block_limits.max_serialized_size);
            const auto block =
                encoded_block.has_value()
                    ? primitives::Block::Deserialize(*encoded_block, config_.network->block_limits)
                    : std::nullopt;
            const auto hash =
                block.has_value() ? primitives::ComputeBlockHash(block->header) : std::nullopt;
            if (!height.has_value() || !block.has_value() || !hash.has_value()) {
                return std::nullopt;
            }
            snapshot.active_blocks.push_back(ExplorerBlock{*height, *hash, *block});
        }
        return reader.RequireEnd() ? std::optional<ExplorerSnapshot>{std::move(snapshot)}
                                   : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace nova::explorer
