#pragma once

#include "crypto/crypto.hpp"
#include "primitives/block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace nova::consensus
{

// A target is stored canonically as a 256-bit unsigned integer in big-endian
// byte order. This representation is independent of host endianness.
class Target256 final
{
  public:
    static constexpr std::size_t kSize = 32U;
    using Bytes = std::array<std::uint8_t, kSize>;

    Target256() = default;
    explicit Target256(Bytes bytes) noexcept;

    [[nodiscard]] static std::optional<Target256>
    FromBigEndian(std::span<const std::uint8_t> serialized) noexcept;
    [[nodiscard]] static std::optional<Target256> FromHex(std::string_view hex) noexcept;

    [[nodiscard]] const Bytes& bytes() const noexcept;
    [[nodiscard]] bool IsZero() const noexcept;

    friend bool operator==(const Target256&, const Target256&) = default;
    friend bool operator<(const Target256& left, const Target256& right) noexcept;

  private:
    Bytes bytes_{};
};

enum class TargetError : std::uint8_t {
    kNone,
    kNegative,
    kZero,
    kOverflow,
    kNonCanonical,
    kAbovePowLimit,
};

struct TargetDecodeResult final {
    TargetError error{TargetError::kNone};
    std::optional<Target256> target;
};

struct PowParameters final {
    Target256 pow_limit;
    std::uint32_t pow_limit_compact{};
};

struct MiningResult final {
    primitives::BlockHeader header;
    crypto::Hash256 block_hash;
    std::uint64_t attempts{};
};

[[nodiscard]] const PowParameters& RegtestPowParameters() noexcept;
[[nodiscard]] TargetDecodeResult TargetFromCompact(std::uint32_t compact) noexcept;
[[nodiscard]] TargetDecodeResult TargetFromCompact(std::uint32_t compact,
                                                   const PowParameters& parameters) noexcept;
[[nodiscard]] std::optional<std::uint32_t> CompactFromTarget(const Target256& target) noexcept;
[[nodiscard]] TargetError ValidateTarget(const Target256& target,
                                         const Target256& pow_limit) noexcept;
[[nodiscard]] bool CheckProofOfWork(const crypto::Hash256& block_hash,
                                    const Target256& target) noexcept;
[[nodiscard]] std::optional<Target256> CalculateWork(const Target256& target) noexcept;
[[nodiscard]] std::optional<MiningResult> MineRegtestBlock(primitives::BlockHeader candidate,
                                                           std::uint64_t maximum_attempts) noexcept;

} // namespace nova::consensus
