#include "explorer/http_response_parser.hpp"

#include <limits>
#include <optional>
#include <string_view>

namespace nova::explorer::detail
{
namespace
{

[[nodiscard]] bool StartsWithAsciiCaseInsensitive(const std::string_view value,
                                                  const std::string_view prefix) noexcept
{
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0U; i < prefix.size(); ++i) {
        auto character = static_cast<unsigned char>(value[i]);
        if (character >= static_cast<unsigned char>('A') &&
            character <= static_cast<unsigned char>('Z')) {
            character = static_cast<unsigned char>(character + ('a' - 'A'));
        }
        if (character != static_cast<unsigned char>(prefix[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::size_t> ContentLength(const std::string_view headers) noexcept
{
    constexpr std::string_view kName{"content-length:"};
    constexpr std::string_view kTransferEncodingName{"transfer-encoding:"};
    std::optional<std::size_t> result;
    std::size_t position{};
    while (position < headers.size()) {
        const auto line_end = headers.find("\r\n", position);
        const auto end = line_end == std::string_view::npos ? headers.size() : line_end;
        const auto line = headers.substr(position, end - position);
        if (StartsWithAsciiCaseInsensitive(line, kTransferEncodingName)) {
            return std::nullopt;
        }
        if (StartsWithAsciiCaseInsensitive(line, kName)) {
            if (result.has_value()) {
                return std::nullopt;
            }
            const auto value = line.substr(kName.size());
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

} // namespace

std::optional<std::string_view>
ParseRpcSnapshotHttpResponse(const std::string_view response,
                             const std::size_t max_response_bytes) noexcept
{
    if (response.size() > max_response_bytes) {
        return std::nullopt;
    }
    const auto headers_end = response.find("\r\n\r\n");
    if (headers_end == std::string_view::npos || !response.starts_with("HTTP/1.1 200 ")) {
        return std::nullopt;
    }
    const auto length = ContentLength(response.substr(0U, headers_end));
    const auto body_offset = headers_end + 4U;
    if (!length.has_value() || *length > max_response_bytes ||
        *length != response.size() - body_offset) {
        return std::nullopt;
    }
    return response.substr(body_offset, *length);
}

std::optional<std::string_view> ExtractRpcSnapshotHex(const std::string_view body,
                                                      const std::size_t max_body_bytes) noexcept
{
    constexpr std::string_view kPrefix{
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"encoding\":\"nova-snapshot-v1\",\"data\":\""};
    constexpr std::string_view kSuffix{"\"}}"};
    if (body.size() > max_body_bytes || body.size() < kPrefix.size() + kSuffix.size() ||
        !body.starts_with(kPrefix) || !body.ends_with(kSuffix)) {
        return std::nullopt;
    }
    return body.substr(kPrefix.size(), body.size() - kPrefix.size() - kSuffix.size());
}

} // namespace nova::explorer::detail
