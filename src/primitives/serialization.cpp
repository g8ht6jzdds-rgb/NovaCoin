#include "primitives/serialization.hpp"

#include <algorithm>
#include <bit>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace nova::primitives
{
namespace
{

template <typename Unsigned>
[[nodiscard]] bool WriteLittleEndian(BinaryWriter& writer, const Unsigned value) noexcept
{
    static_assert(std::is_unsigned_v<Unsigned>);
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        const auto byte = static_cast<std::uint8_t>(value >> shift);
        if (!writer.WriteU8(byte)) {
            return false;
        }
    }
    return true;
}

template <typename Unsigned>
[[nodiscard]] std::optional<Unsigned> ReadLittleEndian(BinaryReader& reader) noexcept
{
    static_assert(std::is_unsigned_v<Unsigned>);
    Unsigned result = 0;
    for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
        const auto byte = reader.ReadU8();
        if (!byte.has_value()) {
            return std::nullopt;
        }
        const auto shift = static_cast<unsigned>(index * 8U);
        result |= static_cast<Unsigned>(*byte) << shift;
    }
    return result;
}

} // namespace

bool BinaryWriter::WriteU8(const std::uint8_t value) noexcept
{
    return AppendByte(value);
}

bool BinaryWriter::WriteU16(const std::uint16_t value) noexcept
{
    return WriteLittleEndian(*this, value);
}

bool BinaryWriter::WriteU32(const std::uint32_t value) noexcept
{
    return WriteLittleEndian(*this, value);
}

bool BinaryWriter::WriteU64(const std::uint64_t value) noexcept
{
    return WriteLittleEndian(*this, value);
}

bool BinaryWriter::WriteI64(const std::int64_t value) noexcept
{
    return WriteU64(std::bit_cast<std::uint64_t>(value));
}

bool BinaryWriter::WriteCompactSize(const std::uint64_t value) noexcept
{
    if (value < 253U) {
        return WriteU8(static_cast<std::uint8_t>(value));
    }
    if (value <= std::numeric_limits<std::uint16_t>::max()) {
        return WriteU8(0xFDU) && WriteU16(static_cast<std::uint16_t>(value));
    }
    if (value <= std::numeric_limits<std::uint32_t>::max()) {
        return WriteU8(0xFEU) && WriteU32(static_cast<std::uint32_t>(value));
    }
    return WriteU8(0xFFU) && WriteU64(value);
}

bool BinaryWriter::WriteBytes(const std::span<const std::uint8_t> bytes,
                              const std::uint64_t maximum_size) noexcept
{
    if (!FitsInUint64(bytes.size()) || bytes.size() > maximum_size) {
        return false;
    }
    return WriteCompactSize(static_cast<std::uint64_t>(bytes.size())) && Append(bytes);
}

bool BinaryWriter::WriteString(const std::string_view value,
                               const std::uint64_t maximum_size) noexcept
{
    constexpr std::uint8_t kEmptyString = 0U;
    const auto* data =
        value.empty() ? &kEmptyString : reinterpret_cast<const std::uint8_t*>(value.data());
    return WriteBytes(std::span<const std::uint8_t>{data, value.size()}, maximum_size);
}

bool BinaryWriter::WriteHash256(const crypto::Hash256& hash) noexcept
{
    return Append(hash.bytes());
}

std::span<const std::uint8_t> BinaryWriter::bytes() const noexcept
{
    return bytes_;
}

bool BinaryWriter::FitsInUint64(const std::size_t value) noexcept
{
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        return value <= std::numeric_limits<std::uint64_t>::max();
    }
    return true;
}

bool BinaryWriter::Append(const std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.empty()) {
        return true;
    }
    if (bytes.size() > std::numeric_limits<std::size_t>::max() - bytes_.size()) {
        return false;
    }
    try {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    return true;
}

bool BinaryWriter::AppendByte(const std::uint8_t byte) noexcept
{
    try {
        bytes_.push_back(byte);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
    return true;
}

BinaryReader::BinaryReader(const std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

std::optional<std::uint8_t> BinaryReader::ReadU8() noexcept
{
    if (!CanRead(1U)) {
        return std::nullopt;
    }
    return bytes_[offset_++];
}

std::optional<std::uint16_t> BinaryReader::ReadU16() noexcept
{
    return ReadLittleEndian<std::uint16_t>(*this);
}

std::optional<std::uint32_t> BinaryReader::ReadU32() noexcept
{
    return ReadLittleEndian<std::uint32_t>(*this);
}

std::optional<std::uint64_t> BinaryReader::ReadU64() noexcept
{
    return ReadLittleEndian<std::uint64_t>(*this);
}

std::optional<std::int64_t> BinaryReader::ReadI64() noexcept
{
    const auto value = ReadU64();
    if (!value.has_value()) {
        return std::nullopt;
    }
    return std::bit_cast<std::int64_t>(*value);
}

std::optional<std::uint64_t>
BinaryReader::ReadCompactSize(const std::uint64_t maximum_value) noexcept
{
    const auto prefix = ReadU8();
    if (!prefix.has_value()) {
        return std::nullopt;
    }
    if (*prefix < 0xFDU) {
        if (*prefix > maximum_value) {
            SetError(DeserializationError::kLimitExceeded);
            return std::nullopt;
        }
        return *prefix;
    }

    std::optional<std::uint64_t> value;
    std::uint64_t minimum_value = 0;
    if (*prefix == 0xFDU) {
        const auto decoded = ReadU16();
        if (decoded.has_value()) {
            value = *decoded;
        }
        minimum_value = 253U;
    } else if (*prefix == 0xFEU) {
        const auto decoded = ReadU32();
        if (decoded.has_value()) {
            value = *decoded;
        }
        minimum_value = 65'536U;
    } else {
        value = ReadU64();
        minimum_value = 4'294'967'296ULL;
    }
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value < minimum_value) {
        SetError(DeserializationError::kNonCanonicalCompactSize);
        return std::nullopt;
    }
    if (*value > maximum_value) {
        SetError(DeserializationError::kLimitExceeded);
        return std::nullopt;
    }
    return value;
}

std::optional<std::vector<std::uint8_t>>
BinaryReader::ReadBytes(const std::uint64_t maximum_size) noexcept
{
    const auto size = ReadCompactSize(maximum_size);
    if (!size.has_value()) {
        return std::nullopt;
    }
    if (*size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        SetError(DeserializationError::kIntegerOverflow);
        return std::nullopt;
    }
    const auto byte_count = static_cast<std::size_t>(*size);
    if (!CanRead(byte_count)) {
        return std::nullopt;
    }
    if (byte_count == 0U) {
        return std::vector<std::uint8_t>{};
    }

    try {
        std::vector<std::uint8_t> result;
        result.reserve(byte_count);
        result.insert(result.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                      bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + byte_count));
        offset_ += byte_count;
        return result;
    } catch (const std::bad_alloc&) {
        SetError(DeserializationError::kAllocationFailure);
        return std::nullopt;
    } catch (const std::length_error&) {
        SetError(DeserializationError::kIntegerOverflow);
        return std::nullopt;
    }
}

std::optional<std::string> BinaryReader::ReadString(const std::uint64_t maximum_size) noexcept
{
    const auto bytes = ReadBytes(maximum_size);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    try {
        return std::string{bytes->begin(), bytes->end()};
    } catch (const std::bad_alloc&) {
        SetError(DeserializationError::kAllocationFailure);
        return std::nullopt;
    } catch (const std::length_error&) {
        SetError(DeserializationError::kIntegerOverflow);
        return std::nullopt;
    }
}

std::optional<crypto::Hash256> BinaryReader::ReadHash256() noexcept
{
    const auto bytes = ReadFixedBytes<crypto::Hash256::kSize>();
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    return crypto::Hash256{*bytes};
}

bool BinaryReader::RequireEnd() noexcept
{
    if (error_ != DeserializationError::kNone) {
        return false;
    }
    if (remaining() != 0U) {
        SetError(DeserializationError::kTrailingBytes);
        return false;
    }
    return true;
}

DeserializationError BinaryReader::error() const noexcept
{
    return error_;
}

std::size_t BinaryReader::remaining() const noexcept
{
    return bytes_.size() - offset_;
}

bool BinaryReader::CanRead(const std::size_t count) noexcept
{
    if (count > remaining()) {
        SetError(DeserializationError::kTruncated);
        return false;
    }
    return true;
}

void BinaryReader::SetError(const DeserializationError error) noexcept
{
    if (error_ == DeserializationError::kNone) {
        error_ = error;
    }
}

} // namespace nova::primitives
