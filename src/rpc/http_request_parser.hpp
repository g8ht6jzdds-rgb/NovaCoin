#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nova::rpc::detail
{

struct ParsedHttpRequest final {
    std::string_view method;
    std::string_view authorization;
    std::string_view body;
};

enum class HttpParseState : std::uint8_t { kIncomplete, kReady, kInvalid };

[[nodiscard]] HttpParseState ParseHttpRequest(std::string_view input, std::size_t max_header_bytes,
                                              std::size_t max_body_bytes,
                                              ParsedHttpRequest& result) noexcept;

} // namespace nova::rpc::detail
