#include "primitives/block.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    constexpr std::size_t kMaximumInput = 1U << 20U;
    constexpr nova::primitives::TransactionLimits kTransactionLimits{
        21'000'000LL * 100'000'000LL, 1'000'000U, 1'024U, 1'024U, 10'000U};
    constexpr nova::primitives::BlockLimits kLimits{kTransactionLimits, 1'000'000U, 1'024U,
                                                    10'000U};
    if (size > kMaximumInput) {
        return 0;
    }
    constexpr std::uint8_t kEmpty = 0U;
    const auto* input = size == 0U ? &kEmpty : data;
    static_cast<void>(
        nova::primitives::Block::Deserialize(std::span<const std::uint8_t>{input, size}, kLimits));
    return 0;
}
