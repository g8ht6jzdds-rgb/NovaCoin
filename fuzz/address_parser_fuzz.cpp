#include "explorer/explorer.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    constexpr std::size_t kMaximumInput = 4'096U;
    if (size > kMaximumInput) {
        return 0;
    }
    constexpr std::uint8_t kEmpty = 0U;
    const auto* input = size == 0U ? &kEmpty : data;
    static_cast<void>(
        nova::explorer::ExtractP2pkhAddress(std::span<const std::uint8_t>{input, size}));
    return 0;
}
