#include "rpc/http_limits.hpp"
#include "rpc/http_request_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace
{

constexpr std::size_t kMaxHeaderBytes = nova::rpc::kNodeRpcHttpHeaderBytes;
constexpr std::size_t kMaxBodyBytes = nova::rpc::kNodeRpcHttpBodyBytes;
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
    constexpr std::string_view kSentinel{"fuzz-sentinel"};
    nova::rpc::detail::ParsedHttpRequest result{kSentinel, kSentinel, kSentinel};
    const auto state =
        nova::rpc::detail::ParseHttpRequest(input, kMaxHeaderBytes, kMaxBodyBytes, result);

    if (state == nova::rpc::detail::HttpParseState::kReady) {
        if (result.method != "POST" || result.body.size() > kMaxBodyBytes ||
            result.authorization.size() > kMaxHeaderBytes) {
            std::abort();
        }
    } else if (result.method != kSentinel || result.authorization != kSentinel ||
               result.body != kSentinel) {
        std::abort();
    }
    return 0;
}
