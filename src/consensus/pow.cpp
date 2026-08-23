#include "consensus/pow.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace nova::consensus
{
namespace
{

[[nodiscard]] Target256 TargetFromLittleEndianHash(const crypto::Hash256& hash) noexcept
{
    Target256::Bytes target_bytes{};
    const auto& hash_bytes = hash.bytes();
    std::reverse_copy(hash_bytes.begin(), hash_bytes.end(), target_bytes.begin());
    return Target256{target_bytes};
}

[[nodiscard]] bool AddOne(std::array<std::uint8_t, 33U>& value) noexcept
{
    for (std::size_t index = value.size(); index > 0U; --index) {
        const auto byte_index = index - 1U;
        if (value.at(byte_index) != std::numeric_limits<std::uint8_t>::max()) {
            ++value.at(byte_index);
            return true;
        }
        value.at(byte_index) = 0U;
    }
    return false;
}

[[nodiscard]] bool GreaterOrEqual(const std::array<std::uint8_t, 33U>& left,
                                  const std::array<std::uint8_t, 33U>& right) noexcept
{
    return !std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
}

void Subtract(std::array<std::uint8_t, 33U>& left,
              const std::array<std::uint8_t, 33U>& right) noexcept
{
    std::uint16_t borrow = 0U;
    for (std::size_t index = left.size(); index > 0U; --index) {
        const auto byte_index = index - 1U;
        const auto minuend = static_cast<std::uint16_t>(left.at(byte_index));
        const auto subtrahend = static_cast<std::uint16_t>(right.at(byte_index)) + borrow;
        if (minuend < subtrahend) {
            const auto difference =
                static_cast<std::uint32_t>(minuend) + 256U - static_cast<std::uint32_t>(subtrahend);
            left.at(byte_index) = static_cast<std::uint8_t>(difference);
            borrow = 1U;
        } else {
            left.at(byte_index) = static_cast<std::uint8_t>(minuend - subtrahend);
            borrow = 0U;
        }
    }
}

void ShiftLeftAndAppendBit(std::array<std::uint8_t, 33U>& value, const bool bit) noexcept
{
    std::uint8_t carry = static_cast<std::uint8_t>(bit ? 1U : 0U);
    for (std::size_t index = value.size(); index > 0U; --index) {
        const auto byte_index = index - 1U;
        const auto next_carry = static_cast<std::uint8_t>((value.at(byte_index) >> 7U) & 1U);
        value.at(byte_index) = static_cast<std::uint8_t>((value.at(byte_index) << 1U) | carry);
        carry = next_carry;
    }
}

[[nodiscard]] Target256 DivideForWork(const Target256::Bytes& numerator,
                                      const std::array<std::uint8_t, 33U>& denominator) noexcept
{
    Target256::Bytes quotient{};
    std::array<std::uint8_t, 33U> remainder{};

    for (std::size_t byte_index = 0U; byte_index < numerator.size(); ++byte_index) {
        for (std::uint8_t bit_index = 8U; bit_index > 0U; --bit_index) {
            const auto shift = static_cast<std::uint8_t>(bit_index - 1U);
            const bool input_bit = ((numerator.at(byte_index) >> shift) & 1U) != 0U;
            ShiftLeftAndAppendBit(remainder, input_bit);
            if (GreaterOrEqual(remainder, denominator)) {
                Subtract(remainder, denominator);
                quotient.at(byte_index) = static_cast<std::uint8_t>(
                    quotient.at(byte_index) | static_cast<std::uint8_t>(1U << shift));
            }
        }
    }
    return Target256{quotient};
}

[[nodiscard]] bool IncrementTargetBytes(Target256::Bytes& value) noexcept
{
    for (std::size_t index = value.size(); index > 0U; --index) {
        const auto byte_index = index - 1U;
        if (value.at(byte_index) != std::numeric_limits<std::uint8_t>::max()) {
            ++value.at(byte_index);
            return true;
        }
        value.at(byte_index) = 0U;
    }
    return false;
}

} // namespace

Target256::Target256(Bytes bytes) noexcept : bytes_(bytes) {}

std::optional<Target256>
Target256::FromBigEndian(const std::span<const std::uint8_t> serialized) noexcept
{
    if (serialized.size() != kSize) {
        return std::nullopt;
    }
    Bytes bytes{};
    std::copy(serialized.begin(), serialized.end(), bytes.begin());
    return Target256{bytes};
}

std::optional<Target256> Target256::FromHex(const std::string_view hex) noexcept
{
    const auto hash = crypto::Hash256::FromHex(hex);
    if (!hash.has_value()) {
        return std::nullopt;
    }
    return Target256{hash->bytes()};
}

const Target256::Bytes& Target256::bytes() const noexcept
{
    return bytes_;
}

bool Target256::IsZero() const noexcept
{
    return std::all_of(bytes_.begin(), bytes_.end(),
                       [](const std::uint8_t byte) { return byte == 0U; });
}

bool operator<(const Target256& left, const Target256& right) noexcept
{
    return std::lexicographical_compare(left.bytes_.begin(), left.bytes_.end(),
                                        right.bytes_.begin(), right.bytes_.end());
}

const PowParameters& RegtestPowParameters() noexcept
{
    static constexpr Target256::Bytes kRegtestPowLimit{
        0x7FU, 0xFFU, 0xFFU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        0U,    0U,    0U,    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    static const PowParameters kParameters{Target256{kRegtestPowLimit}, 0x207F'FFFFU};
    return kParameters;
}

TargetDecodeResult TargetFromCompact(const std::uint32_t compact) noexcept
{
    const auto size = static_cast<std::uint8_t>(compact >> 24U);
    const auto mantissa = compact & 0x007F'FFFFU;
    if ((compact & 0x0080'0000U) != 0U) {
        return {TargetError::kNegative, std::nullopt};
    }
    if (mantissa == 0U) {
        return {TargetError::kZero, std::nullopt};
    }
    if (size > 34U || (mantissa > 0xFFU && size > 33U) || (mantissa > 0xFFFFU && size > 32U)) {
        return {TargetError::kOverflow, std::nullopt};
    }

    Target256::Bytes target_bytes{};
    if (size <= 3U) {
        const auto value = mantissa >> (8U * static_cast<std::uint32_t>(3U - size));
        for (std::size_t index = 0U; index < 4U; ++index) {
            target_bytes.at(Target256::kSize - 1U - index) = static_cast<std::uint8_t>(
                (value >> static_cast<std::uint32_t>(8U * index)) & 0xFFU);
        }
    } else {
        const auto offset = Target256::kSize - static_cast<std::size_t>(size);
        target_bytes.at(offset) = static_cast<std::uint8_t>(mantissa >> 16U);
        target_bytes.at(offset + 1U) = static_cast<std::uint8_t>(mantissa >> 8U);
        target_bytes.at(offset + 2U) = static_cast<std::uint8_t>(mantissa);
    }

    const Target256 target{target_bytes};
    if (target.IsZero()) {
        return {TargetError::kZero, std::nullopt};
    }
    const auto canonical_compact = CompactFromTarget(target);
    if (!canonical_compact.has_value() || *canonical_compact != compact) {
        return {TargetError::kNonCanonical, std::nullopt};
    }
    return {TargetError::kNone, target};
}

TargetDecodeResult TargetFromCompact(const std::uint32_t compact,
                                     const PowParameters& parameters) noexcept
{
    const auto decoded = TargetFromCompact(compact);
    if (decoded.error != TargetError::kNone || !decoded.target.has_value()) {
        return decoded;
    }
    const auto target_error = ValidateTarget(*decoded.target, parameters.pow_limit);
    if (target_error != TargetError::kNone) {
        return {target_error, std::nullopt};
    }
    return decoded;
}

std::optional<std::uint32_t> CompactFromTarget(const Target256& target) noexcept
{
    if (target.IsZero()) {
        return std::nullopt;
    }
    const auto first_nonzero = std::find_if(target.bytes().begin(), target.bytes().end(),
                                            [](const std::uint8_t byte) { return byte != 0U; });
    const auto size = static_cast<std::size_t>(target.bytes().end() - first_nonzero);
    std::uint32_t mantissa = 0U;
    if (size <= 3U) {
        for (std::size_t index = 0U; index < size; ++index) {
            mantissa |=
                static_cast<std::uint32_t>(*(first_nonzero + static_cast<std::ptrdiff_t>(index)))
                << static_cast<std::uint32_t>(8U * (2U - index));
        }
    } else {
        mantissa = (static_cast<std::uint32_t>(*first_nonzero) << 16U) |
                   (static_cast<std::uint32_t>(*(first_nonzero + 1U)) << 8U) |
                   static_cast<std::uint32_t>(*(first_nonzero + 2U));
    }

    auto encoded_size = static_cast<std::uint32_t>(size);
    if ((mantissa & 0x0080'0000U) != 0U) {
        mantissa >>= 8U;
        ++encoded_size;
    }
    return (encoded_size << 24U) | mantissa;
}

TargetError ValidateTarget(const Target256& target, const Target256& pow_limit) noexcept
{
    if (target.IsZero()) {
        return TargetError::kZero;
    }
    if (pow_limit.IsZero() || pow_limit < target) {
        return TargetError::kAbovePowLimit;
    }
    return TargetError::kNone;
}

bool CheckProofOfWork(const crypto::Hash256& block_hash, const Target256& target) noexcept
{
    if (target.IsZero()) {
        return false;
    }
    const auto hash_value = TargetFromLittleEndianHash(block_hash);
    return !(target < hash_value);
}

std::optional<Target256> CalculateWork(const Target256& target) noexcept
{
    if (target.IsZero()) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 33U> denominator{};
    std::copy(target.bytes().begin(), target.bytes().end(), denominator.begin() + 1U);
    if (!AddOne(denominator)) {
        return std::nullopt;
    }

    Target256::Bytes numerator{};
    std::transform(target.bytes().begin(), target.bytes().end(), numerator.begin(),
                   [](const std::uint8_t byte) { return static_cast<std::uint8_t>(~byte); });
    const auto quotient = DivideForWork(numerator, denominator);
    auto work = quotient.bytes();
    if (!IncrementTargetBytes(work)) {
        return std::nullopt;
    }
    return Target256{work};
}

std::optional<MiningResult> MineRegtestBlock(primitives::BlockHeader candidate,
                                             const std::uint64_t maximum_attempts) noexcept
{
    const auto& parameters = RegtestPowParameters();
    candidate.bits = parameters.pow_limit_compact;
    const auto attempts = std::min(maximum_attempts, std::uint64_t{1} << 32U);
    const auto starting_nonce = candidate.nonce;
    const auto& target = parameters.pow_limit;
    for (std::uint64_t attempt = 0U; attempt < attempts; ++attempt) {
        candidate.nonce = starting_nonce + static_cast<std::uint32_t>(attempt);
        const auto hash = primitives::ComputeBlockHash(candidate);
        if (!hash.has_value()) {
            return std::nullopt;
        }
        if (CheckProofOfWork(*hash, target)) {
            return MiningResult{candidate, *hash, attempt + 1U};
        }
    }
    return std::nullopt;
}

} // namespace nova::consensus
