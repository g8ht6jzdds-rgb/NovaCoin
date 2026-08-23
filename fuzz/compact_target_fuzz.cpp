#include "consensus/pow.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 4U) {
        return 0;
    }
    const auto compact =
        static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
        (static_cast<std::uint32_t>(data[2]) << 16U) | (static_cast<std::uint32_t>(data[3]) << 24U);
    static_cast<void>(nova::consensus::TargetFromCompact(compact));
    static_cast<void>(
        nova::consensus::TargetFromCompact(compact, nova::consensus::RegtestPowParameters()));
    return 0;
}
