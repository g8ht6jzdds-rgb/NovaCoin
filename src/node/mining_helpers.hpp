#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nova::node
{

// Encodes the existing consensus height prefix for a locally built coinbase.
// The validator remains authoritative for the complete block.
[[nodiscard]] inline std::vector<std::uint8_t> CoinbaseHeightScript(std::uint32_t height)
{
    if (height == 0U)
        return {0U};
    std::array<std::uint8_t, 5U> encoded{};
    std::size_t size{};
    // A uint32_t needs at most four magnitude bytes; reserve the fifth for
    // its sign bit. The explicit bound also makes every array access local.
    while (height != 0U && size < encoded.size() - 1U) {
        encoded[size++] = static_cast<std::uint8_t>(height & 0xFFU);
        height >>= 8U;
    }
    if (size != 0U && (encoded[size - 1U] & 0x80U) != 0U)
        encoded[size++] = 0U;
    std::vector<std::uint8_t> script;
    script.reserve(size + 1U);
    script.push_back(static_cast<std::uint8_t>(size));
    script.insert(script.end(), encoded.begin(),
                  encoded.begin() + static_cast<std::ptrdiff_t>(size));
    return script;
}
} // namespace nova::node
