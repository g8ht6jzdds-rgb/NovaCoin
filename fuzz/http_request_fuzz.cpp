#include "rpc/http_request_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace
{

constexpr std::size_t kMaxHeaderBytes = 256U;
constexpr std::size_t kMaxBodyBytes = 8'192U;
constexpr std::size_t kMaxInputBytes = kMaxHeaderBytes + kMaxBodyBytes;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    if (size > kMaxInputBytes) {
        return 0;
    }

    const std::string_view input =
        size == 0U ? std::string_view{}
                   : std::string_view{reinterpret_cast<const char*>(data), size};
    nova::rpc::detail::ParsedHttpRequest result{};
    const auto state =
        nova::rpc::detail::ParseHttpRequest(input, kMaxHeaderBytes, kMaxBodyBytes, result);

    if (state == nova::rpc::detail::HttpParseState::kReady &&
        (result.method != "POST" || result.body.size() > kMaxBodyBytes ||
         result.authorization.size() > kMaxHeaderBytes)) {
        std::abort();
    }
    return 0;
}
