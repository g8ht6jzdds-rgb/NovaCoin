#include "crypto/crypto.hpp"

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
    const std::span<const std::uint8_t> bytes{input, size};
    static_cast<void>(nova::crypto::Signature::FromDer(bytes));
    static_cast<void>(nova::crypto::PublicKey::FromCompressed(bytes));
    return 0;
}
