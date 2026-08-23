#pragma once

#include "crypto/crypto.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nova::primitives
{

enum class DeserializationError : std::uint8_t {
    kNone,
    kTruncated,
    kNonCanonicalCompactSize,
    kLimitExceeded,
    kIntegerOverflow,
    kAllocationFailure,
    kCallbackFailure,
    kTrailingBytes,
};

class BinaryWriter final
{
  public:
    [[nodiscard]] bool WriteU8(std::uint8_t value) noexcept;
    [[nodiscard]] bool WriteU16(std::uint16_t value) noexcept;
    [[nodiscard]] bool WriteU32(std::uint32_t value) noexcept;
    [[nodiscard]] bool WriteU64(std::uint64_t value) noexcept;
    [[nodiscard]] bool WriteI64(std::int64_t value) noexcept;
    [[nodiscard]] bool WriteCompactSize(std::uint64_t value) noexcept;

    template <std::size_t Size>
    [[nodiscard]] bool WriteFixedBytes(const std::array<std::uint8_t, Size>& bytes) noexcept
    {
        return Append(bytes);
    }

    [[nodiscard]] bool WriteBytes(std::span<const std::uint8_t> bytes,
                                  std::uint64_t maximum_size) noexcept;
    [[nodiscard]] bool WriteString(std::string_view value, std::uint64_t maximum_size) noexcept;
    [[nodiscard]] bool WriteHash256(const crypto::Hash256& hash) noexcept;

    template <typename T, typename SerializeElement>
    [[nodiscard]] bool WriteArray(const std::span<const T> values,
                                  const std::uint64_t maximum_count,
                                  SerializeElement serialize_element) noexcept
    {
        if (!FitsInUint64(values.size()) || values.size() > maximum_count) {
            return false;
        }
        if (!WriteCompactSize(static_cast<std::uint64_t>(values.size()))) {
            return false;
        }
        try {
            for (const auto& value : values) {
                if (!serialize_element(*this, value)) {
                    return false;
                }
            }
        } catch (...) {
            return false;
        }
        return true;
    }

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

  private:
    [[nodiscard]] static bool FitsInUint64(std::size_t value) noexcept;
    [[nodiscard]] bool Append(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] bool AppendByte(std::uint8_t byte) noexcept;

    std::vector<std::uint8_t> bytes_;
};

class BinaryReader final
{
  public:
    explicit BinaryReader(std::span<const std::uint8_t> bytes) noexcept;

    [[nodiscard]] std::optional<std::uint8_t> ReadU8() noexcept;
    [[nodiscard]] std::optional<std::uint16_t> ReadU16() noexcept;
    [[nodiscard]] std::optional<std::uint32_t> ReadU32() noexcept;
    [[nodiscard]] std::optional<std::uint64_t> ReadU64() noexcept;
    [[nodiscard]] std::optional<std::int64_t> ReadI64() noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    ReadCompactSize(std::uint64_t maximum_value) noexcept;

    template <std::size_t Size>
    [[nodiscard]] std::optional<std::array<std::uint8_t, Size>> ReadFixedBytes() noexcept
    {
        if (!CanRead(Size)) {
            return std::nullopt;
        }
        std::array<std::uint8_t, Size> result{};
        for (std::size_t index = 0; index < Size; ++index) {
            result.at(index) = bytes_[offset_ + index];
        }
        offset_ += Size;
        return result;
    }

    [[nodiscard]] std::optional<std::vector<std::uint8_t>>
    ReadBytes(std::uint64_t maximum_size) noexcept;
    [[nodiscard]] std::optional<std::string> ReadString(std::uint64_t maximum_size) noexcept;
    [[nodiscard]] std::optional<crypto::Hash256> ReadHash256() noexcept;

    template <typename T, typename DeserializeElement>
    [[nodiscard]] std::optional<std::vector<T>>
    ReadArray(const std::uint64_t maximum_count, const std::size_t minimum_element_size,
              DeserializeElement deserialize_element) noexcept
    {
        const auto count = ReadCompactSize(maximum_count);
        if (!count.has_value()) {
            return std::nullopt;
        }
        if (*count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            SetError(DeserializationError::kIntegerOverflow);
            return std::nullopt;
        }
        const auto element_count = static_cast<std::size_t>(*count);
        if (minimum_element_size != 0U && element_count > remaining() / minimum_element_size) {
            SetError(DeserializationError::kTruncated);
            return std::nullopt;
        }

        std::vector<T> result;
        try {
            result.reserve(element_count);
            for (std::size_t index = 0; index < element_count; ++index) {
                const auto value = deserialize_element(*this);
                if (!value.has_value()) {
                    return std::nullopt;
                }
                result.push_back(std::move(*value));
            }
        } catch (const std::bad_alloc&) {
            SetError(DeserializationError::kAllocationFailure);
            return std::nullopt;
        } catch (const std::length_error&) {
            SetError(DeserializationError::kIntegerOverflow);
            return std::nullopt;
        } catch (...) {
            SetError(DeserializationError::kCallbackFailure);
            return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] bool RequireEnd() noexcept;
    [[nodiscard]] DeserializationError error() const noexcept;
    [[nodiscard]] std::size_t remaining() const noexcept;

  private:
    [[nodiscard]] bool CanRead(std::size_t count) noexcept;
    void SetError(DeserializationError error) noexcept;

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
    DeserializationError error_{DeserializationError::kNone};
};

} // namespace nova::primitives
