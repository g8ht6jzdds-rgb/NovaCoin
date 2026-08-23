#include "consensus/network_params.hpp"
#include "primitives/serialization.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace
{

[[nodiscard]] std::optional<std::uint32_t> ParseU32(const std::string_view text)
{
    std::uint32_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
               ? std::optional<std::uint32_t>{value}
               : std::nullopt;
}

[[nodiscard]] std::optional<nova::primitives::Amount> ParseAmount(const std::string_view text)
{
    nova::primitives::Amount value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
               ? std::optional<nova::primitives::Amount>{value}
               : std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> ParseCompactTarget(const std::string_view text)
{
    if (text.size() != 8U) {
        return std::nullopt;
    }
    std::uint32_t value{};
    for (const auto character : text) {
        std::uint32_t nibble{};
        if (character >= '0' && character <= '9') {
            nibble = static_cast<std::uint32_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            nibble = static_cast<std::uint32_t>(character - 'a' + 10);
        } else if (character >= 'A' && character <= 'F') {
            nibble = static_cast<std::uint32_t>(character - 'A' + 10);
        } else {
            return std::nullopt;
        }
        value = (value << 4U) | nibble;
    }
    return value;
}

[[nodiscard]] std::string Hex(const std::span<const std::uint8_t> bytes)
{
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

} // namespace

int main(const int argc, char* argv[])
{
    if (argc != 9) {
        std::cerr << "usage: nova-genesis --timestamp <u32> --message <ASCII> "
                     "--target <compact-hex> --reward <amount>\n";
        return 2;
    }
    std::optional<std::string_view> timestamp;
    std::optional<std::string_view> message;
    std::optional<std::string_view> target;
    std::optional<std::string_view> reward;
    for (int index = 1; index < argc; index += 2) {
        const std::string_view key{argv[index]};
        const std::string_view value{argv[index + 1]};
        if (key == "--timestamp" && !timestamp.has_value()) {
            timestamp = value;
        } else if (key == "--message" && !message.has_value()) {
            message = value;
        } else if (key == "--target" && !target.has_value()) {
            target = value;
        } else if (key == "--reward" && !reward.has_value()) {
            reward = value;
        } else {
            std::cerr << "invalid or duplicate option\n";
            return 2;
        }
    }
    const auto parsed_timestamp = timestamp.has_value() ? ParseU32(*timestamp) : std::nullopt;
    const auto parsed_target = target.has_value() ? ParseCompactTarget(*target) : std::nullopt;
    const auto parsed_reward = reward.has_value() ? ParseAmount(*reward) : std::nullopt;
    if (!parsed_timestamp.has_value() || !message.has_value() || !parsed_target.has_value() ||
        !parsed_reward.has_value()) {
        std::cerr << "invalid genesis argument\n";
        return 2;
    }
    const auto generated = nova::consensus::GenerateGenesis(
        {*parsed_timestamp, *message, *parsed_target, *parsed_reward},
        nova::consensus::RegtestNetworkParams().block_limits, std::uint64_t{1} << 32U);
    if (generated.error != nova::consensus::GenesisError::kNone ||
        !generated.block_hash.has_value() || !generated.merkle_root.has_value() ||
        !generated.block.has_value()) {
        std::cerr << "genesis search failed\n";
        return 1;
    }
    nova::primitives::BinaryWriter writer;
    if (!generated.block->Serialize(writer, nova::consensus::RegtestNetworkParams().block_limits)) {
        std::cerr << "genesis serialization failed\n";
        return 1;
    }
    const auto serialized_digest = nova::crypto::Hash256::DoubleSha256(writer.bytes());
    if (!serialized_digest.has_value()) {
        std::cerr << "genesis serialization digest failed\n";
        return 1;
    }
    std::cout << "nonce=" << generated.block->header.nonce << '\n';
    std::cout << "hash=" << Hex(generated.block_hash->bytes()) << '\n';
    std::cout << "merkle_root=" << Hex(generated.merkle_root->bytes()) << '\n';
    std::cout << "block_hex=" << Hex(writer.bytes()) << '\n';
    std::cout << "block_sha256d=" << Hex(serialized_digest->bytes()) << '\n';
    std::cout << "attempts=" << generated.attempts << '\n';
    return 0;
}
