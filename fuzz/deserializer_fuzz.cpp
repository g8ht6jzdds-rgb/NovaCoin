#include "consensus/pow.hpp"
#include "primitives/block.hpp"
#include "primitives/serialization.hpp"
#include "primitives/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    constexpr std::size_t kMaximumFuzzInput = 1U << 20U;
    if (size > kMaximumFuzzInput) {
        return 0;
    }

    constexpr std::uint8_t kEmptyInput = 0U;
    const auto* input = size == 0U ? &kEmptyInput : data;
    nova::primitives::BinaryReader reader{std::span<const std::uint8_t>{input, size}};
    static_cast<void>(reader.ReadU8());
    static_cast<void>(reader.ReadU16());
    static_cast<void>(reader.ReadU32());
    static_cast<void>(reader.ReadU64());
    static_cast<void>(reader.ReadI64());
    static_cast<void>(reader.ReadHash256());
    static_cast<void>(reader.ReadBytes(4'096U));
    static_cast<void>(reader.ReadString(4'096U));
    static_cast<void>(reader.ReadArray<std::uint32_t>(
        1'024U, 4U,
        [](nova::primitives::BinaryReader& array_reader) { return array_reader.ReadU32(); }));
    static_cast<void>(reader.RequireEnd());

    constexpr nova::primitives::TransactionLimits kFuzzLimits{21'000'000LL * 100'000'000LL,
                                                              1'000'000U, 1'024U, 1'024U, 10'000U};
    static_cast<void>(nova::primitives::Transaction::Deserialize(
        std::span<const std::uint8_t>{input, size}, kFuzzLimits));
    constexpr nova::primitives::BlockLimits kFuzzBlockLimits{kFuzzLimits, 1'000'000U, 1'024U,
                                                             10'000U};
    static_cast<void>(nova::primitives::Block::Deserialize(
        std::span<const std::uint8_t>{input, size}, kFuzzBlockLimits));
    if (size >= sizeof(std::uint32_t)) {
        const auto compact = static_cast<std::uint32_t>(input[0]) |
                             (static_cast<std::uint32_t>(input[1]) << 8U) |
                             (static_cast<std::uint32_t>(input[2]) << 16U) |
                             (static_cast<std::uint32_t>(input[3]) << 24U);
        static_cast<void>(nova::consensus::TargetFromCompact(compact));
    }
    return 0;
}
