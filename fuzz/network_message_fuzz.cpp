#include "net/p2p.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    constexpr std::size_t kMaximumInput = 1U << 20U;
    if (size > kMaximumInput) {
        return 0;
    }
    constexpr nova::primitives::TransactionLimits kTransactionLimits{21'000'000LL * 100'000'000LL,
                                                                     100'000U, 8U, 8U, 128U};
    constexpr nova::primitives::BlockLimits kBlockLimits{kTransactionLimits, 100'000U, 8U, 64U};
    const nova::net::P2PParams parameters{
        0xDAB5'BFFAU, 1, 100'000U, 256U, 16U, 32U, 16U, 16U, kTransactionLimits, kBlockLimits};
    constexpr std::uint8_t kEmptyInput = 0U;
    const auto* input = size == 0U ? &kEmptyInput : data;
    nova::net::MessageParser parser{parameters};
    static_cast<void>(parser.PushBytes(std::span<const std::uint8_t>{input, size}));
    return 0;
}
