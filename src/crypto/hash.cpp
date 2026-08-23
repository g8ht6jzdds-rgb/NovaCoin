#include "crypto/crypto.hpp"

#include <algorithm>

#include <openssl/evp.h>

namespace nova::crypto
{
namespace
{

std::optional<std::uint8_t> HexNibble(const char character) noexcept
{
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    if (character >= 'A' && character <= 'F') {
        return static_cast<std::uint8_t>(character - 'A' + 10);
    }
    return std::nullopt;
}

} // namespace

std::optional<Hash160> Hash160Digest(const std::span<const std::uint8_t> message) noexcept
{
    const auto first = Hash256::Sha256(message);
    if (!first.has_value()) {
        return std::nullopt;
    }
    Hash160 hash{};
    std::size_t output_size = hash.size();
    if (EVP_Q_digest(nullptr, "RIPEMD160", nullptr, first->bytes().data(), first->bytes().size(),
                     hash.data(), &output_size) != 1 ||
        output_size != hash.size()) {
        return std::nullopt;
    }
    return hash;
}

Hash256::Hash256(Bytes bytes) noexcept : bytes_(bytes) {}

std::optional<Hash256> Hash256::FromHex(const std::string_view hex) noexcept
{
    if (hex.size() != kSize * 2U) {
        return std::nullopt;
    }

    Bytes bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto high = HexNibble(hex.at(index * 2U));
        const auto low = HexNibble(hex.at(index * 2U + 1U));
        if (!high.has_value() || !low.has_value()) {
            return std::nullopt;
        }
        bytes.at(index) = static_cast<std::uint8_t>((*high << 4U) | *low);
    }
    return Hash256{bytes};
}

std::optional<Hash256> Hash256::Sha256(const std::span<const std::uint8_t> message) noexcept
{
    Hash256 hash;
    std::size_t output_size = hash.bytes_.size();
    constexpr std::uint8_t kEmptyInput = 0U;
    const auto* input = message.empty() ? &kEmptyInput : message.data();
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, input, message.size(), hash.bytes_.data(),
                     &output_size) != 1 ||
        output_size != hash.bytes_.size()) {
        return std::nullopt;
    }
    return hash;
}

std::optional<Hash256> Hash256::DoubleSha256(const std::span<const std::uint8_t> message) noexcept
{
    const auto first = Sha256(message);
    if (!first.has_value()) {
        return std::nullopt;
    }
    return Sha256(first->bytes());
}

const Hash256::Bytes& Hash256::bytes() const noexcept
{
    return bytes_;
}

} // namespace nova::crypto
