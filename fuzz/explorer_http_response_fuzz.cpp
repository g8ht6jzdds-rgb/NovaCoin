#include "explorer/http_response_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace
{

constexpr std::size_t kMaxInputBytes = nova::explorer::detail::kExplorerRpcSnapshotMaxResponseBytes;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    if (size > kMaxInputBytes) {
        return 0;
    }

    const std::string_view input =
        size == 0U ? std::string_view{}
                   : std::string_view{reinterpret_cast<const char*>(data), size};
    const auto parsed = nova::explorer::detail::ParseRpcSnapshotHttpResponse(input, kMaxInputBytes);
    if (parsed.has_value()) {
        const auto input_start = reinterpret_cast<std::uintptr_t>(input.data());
        const auto body_start = reinterpret_cast<std::uintptr_t>(parsed->data());
        if (body_start < input_start || body_start - input_start > input.size() ||
            parsed->size() > input.size() - (body_start - input_start)) {
            std::abort();
        }
    }
    return 0;
}
