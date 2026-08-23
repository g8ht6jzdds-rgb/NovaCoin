#include "wallet/address.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace nova::wallet
{
namespace
{
constexpr std::string_view kAlphabet{"123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"};
constexpr std::size_t kPayloadSize = 21U;
constexpr std::size_t kEncodedMax = 64U;

[[nodiscard]] std::optional<std::string>
Base58Encode(const std::vector<std::uint8_t>& input) noexcept
{
    try {
        std::size_t zeros{};
        while (zeros < input.size() && input[zeros] == 0U) {
            ++zeros;
        }
        std::vector<std::uint8_t> digits((input.size() - zeros) * 138U / 100U + 1U);
        for (std::size_t i = zeros; i < input.size(); ++i) {
            std::uint32_t carry = input[i];
            for (std::size_t j = digits.size(); j-- > 0U;) {
                carry += static_cast<std::uint32_t>(digits[j]) * 256U;
                digits[j] = static_cast<std::uint8_t>(carry % 58U);
                carry /= 58U;
            }
        }
        auto first = digits.begin();
        while (first != digits.end() && *first == 0U) {
            ++first;
        }
        std::string result(zeros, '1');
        result.reserve(zeros + static_cast<std::size_t>(digits.end() - first));
        for (; first != digits.end(); ++first) {
            result.push_back(kAlphabet[*first]);
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
Base58Decode(const std::string_view text) noexcept
{
    if (text.empty() || text.size() > kEncodedMax) {
        return std::nullopt;
    }
    try {
        std::size_t zeros{};
        while (zeros < text.size() && text[zeros] == '1') {
            ++zeros;
        }
        std::vector<std::uint8_t> bytes((text.size() - zeros) * 733U / 1000U + 1U);
        for (std::size_t i = zeros; i < text.size(); ++i) {
            const auto position = kAlphabet.find(text[i]);
            if (position == std::string_view::npos) {
                return std::nullopt;
            }
            std::uint32_t carry = static_cast<std::uint32_t>(position);
            for (std::size_t j = bytes.size(); j-- > 0U;) {
                carry += static_cast<std::uint32_t>(bytes[j]) * 58U;
                bytes[j] = static_cast<std::uint8_t>(carry % 256U);
                carry /= 256U;
            }
        }
        auto first = bytes.begin();
        while (first != bytes.end() && *first == 0U) {
            ++first;
        }
        std::vector<std::uint8_t> result(zeros, 0U);
        result.insert(result.end(), first, bytes.end());
        return result;
    } catch (...) {
        return std::nullopt;
    }
}
} // namespace

std::optional<std::string> EncodeP2pkhAddress(const crypto::Hash160& key_hash,
                                              const consensus::NetworkParams& network) noexcept
{
    std::vector<std::uint8_t> bytes;
    try {
        bytes.reserve(kPayloadSize + 4U);
        bytes.push_back(network.address_prefixes.p2pkh);
        bytes.insert(bytes.end(), key_hash.begin(), key_hash.end());
    } catch (...) {
        return std::nullopt;
    }
    const auto checksum = crypto::Hash256::DoubleSha256(bytes);
    if (!checksum) {
        return std::nullopt;
    }
    bytes.insert(bytes.end(), checksum->bytes().begin(), checksum->bytes().begin() + 4U);
    return Base58Encode(bytes);
}

std::optional<crypto::Hash160>
DecodeP2pkhAddress(const std::string_view encoded,
                   const consensus::NetworkParams& expected_network) noexcept
{
    const auto bytes = Base58Decode(encoded);
    if (!bytes || bytes->size() != kPayloadSize + 4U ||
        (*bytes)[0] != expected_network.address_prefixes.p2pkh) {
        return std::nullopt;
    }
    const auto checksum =
        crypto::Hash256::DoubleSha256(std::span<const std::uint8_t>{bytes->data(), kPayloadSize});
    if (!checksum || !std::equal(bytes->begin() + static_cast<std::ptrdiff_t>(kPayloadSize),
                                 bytes->end(), checksum->bytes().begin())) {
        return std::nullopt;
    }
    crypto::Hash160 result{};
    std::copy_n(bytes->begin() + 1, result.size(), result.begin());
    return result;
}
} // namespace nova::wallet
