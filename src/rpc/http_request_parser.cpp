#include "rpc/http_request_parser.hpp"

#include <cctype>
#include <limits>
#include <optional>

namespace nova::rpc::detail
{
namespace
{

[[nodiscard]] bool EqualsAsciiCaseInsensitive(const std::string_view value,
                                              const std::string_view lowercase) noexcept
{
    if (value.size() != lowercase.size()) {
        return false;
    }
    for (std::size_t i = 0U; i < value.size(); ++i) {
        auto character = static_cast<unsigned char>(value[i]);
        if (character >= static_cast<unsigned char>('A') &&
            character <= static_cast<unsigned char>('Z')) {
            character = static_cast<unsigned char>(character + ('a' - 'A'));
        }
        if (character != static_cast<unsigned char>(lowercase[i])) {
            return false;
        }
    }
    return true;
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

[[nodiscard]] std::optional<std::size_t> ParseContentLength(const std::string_view value) noexcept
{
    const auto trimmed = Trim(value);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    std::size_t parsed{};
    for (const auto character : trimmed) {
        if (character < '0' || character > '9' ||
            parsed > (std::numeric_limits<std::size_t>::max() - 9U) / 10U) {
            return std::nullopt;
        }
        parsed = parsed * 10U + static_cast<std::size_t>(character - '0');
    }
    return parsed;
}

} // namespace

HttpParseState ParseHttpRequest(const std::string_view input, const std::size_t max_header_bytes,
                                const std::size_t max_body_bytes,
                                ParsedHttpRequest& result) noexcept
{
    const auto header_end = input.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return input.size() > max_header_bytes ? HttpParseState::kInvalid
                                               : HttpParseState::kIncomplete;
    }
    if (header_end + 4U > max_header_bytes) {
        return HttpParseState::kInvalid;
    }
    const std::string_view header{input.data(), header_end};
    const auto first_end = header.find("\r\n");
    if (first_end == std::string_view::npos) {
        return HttpParseState::kInvalid;
    }
    const auto first_line = header.substr(0U, first_end);
    const auto first_space = first_line.find(' ');
    const auto second_space = first_space == std::string_view::npos
                                  ? std::string_view::npos
                                  : first_line.find(' ', first_space + 1U);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
        first_line.find(' ', second_space + 1U) != std::string_view::npos ||
        first_line.substr(second_space + 1U) != "HTTP/1.1") {
        return HttpParseState::kInvalid;
    }
    const auto method = first_line.substr(0U, first_space);
    const auto target = first_line.substr(first_space + 1U, second_space - first_space - 1U);
    if (method != "POST" || (target != "/" && target != "/rpc")) {
        return HttpParseState::kInvalid;
    }
    std::optional<std::size_t> content_length;
    std::optional<std::string_view> authorization;
    std::size_t position = first_end + 2U;
    while (position < header.size()) {
        const auto line_end = header.find("\r\n", position);
        const auto end = line_end == std::string_view::npos ? header.size() : line_end;
        const auto line = header.substr(position, end - position);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            return HttpParseState::kInvalid;
        }
        const auto name = line.substr(0U, colon);
        const auto value = Trim(line.substr(colon + 1U));
        if (EqualsAsciiCaseInsensitive(name, "content-length")) {
            if (content_length.has_value()) {
                return HttpParseState::kInvalid;
            }
            content_length = ParseContentLength(value);
            if (!content_length.has_value() || *content_length > max_body_bytes) {
                return HttpParseState::kInvalid;
            }
        } else if (EqualsAsciiCaseInsensitive(name, "authorization")) {
            if (authorization.has_value() || value.size() > max_header_bytes) {
                return HttpParseState::kInvalid;
            }
            authorization = value;
        } else if (EqualsAsciiCaseInsensitive(name, "transfer-encoding")) {
            return HttpParseState::kInvalid;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        position = line_end + 2U;
    }
    if (!content_length.has_value()) {
        return HttpParseState::kInvalid;
    }
    const auto body_offset = header_end + 4U;
    if (*content_length > std::numeric_limits<std::size_t>::max() - body_offset) {
        return HttpParseState::kInvalid;
    }
    const auto required = body_offset + *content_length;
    if (input.size() < required) {
        return HttpParseState::kIncomplete;
    }
    if (input.size() != required) {
        return HttpParseState::kInvalid;
    }
    result = ParsedHttpRequest{method, authorization.value_or(std::string_view{}),
                               std::string_view{input.data() + body_offset, *content_length}};
    return HttpParseState::kReady;
}

} // namespace nova::rpc::detail
