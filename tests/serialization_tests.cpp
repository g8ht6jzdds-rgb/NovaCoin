#include <gtest/gtest.h>

#include "primitives/serialization.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace
{

using nova::primitives::BinaryReader;
using nova::primitives::BinaryWriter;
using nova::primitives::DeserializationError;

TEST(SerializationScalars, UsesExplicitLittleEndianEncoding)
{
    BinaryWriter writer;
    ASSERT_TRUE(writer.WriteU8(0x12U));
    ASSERT_TRUE(writer.WriteU16(0x3456U));
    ASSERT_TRUE(writer.WriteU32(0x789ABCDEU));
    ASSERT_TRUE(writer.WriteU64(0x0123456789ABCDEFULL));
    ASSERT_TRUE(writer.WriteI64(-2));

    const std::vector<std::uint8_t> expected{0x12U, 0x56U, 0x34U, 0xDEU, 0xBCU, 0x9AU, 0x78U, 0xEFU,
                                             0xCDU, 0xABU, 0x89U, 0x67U, 0x45U, 0x23U, 0x01U, 0xFEU,
                                             0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    EXPECT_TRUE(
        std::equal(writer.bytes().begin(), writer.bytes().end(), expected.begin(), expected.end()));

    BinaryReader reader{writer.bytes()};
    const auto u8 = reader.ReadU8();
    const auto u16 = reader.ReadU16();
    const auto u32 = reader.ReadU32();
    const auto u64 = reader.ReadU64();
    const auto i64 = reader.ReadI64();
    ASSERT_TRUE(u8.has_value());
    ASSERT_TRUE(u16.has_value());
    ASSERT_TRUE(u32.has_value());
    ASSERT_TRUE(u64.has_value());
    ASSERT_TRUE(i64.has_value());
    EXPECT_EQ(*u8, 0x12U);
    EXPECT_EQ(*u16, 0x3456U);
    EXPECT_EQ(*u32, 0x789ABCDEU);
    EXPECT_EQ(*u64, 0x0123456789ABCDEFULL);
    EXPECT_EQ(*i64, -2);
    EXPECT_TRUE(reader.RequireEnd());
}

TEST(SerializationDynamic, RoundTripsBoundedFieldsAndHash)
{
    const std::array<std::uint8_t, 3> fixed{0xAAU, 0xBBU, 0xCCU};
    const std::vector<std::uint8_t> bytes{0x01U, 0x02U, 0x03U};
    const std::vector<std::uint16_t> values{0x1234U, 0xBEEFU};
    const nova::crypto::Hash256::Bytes hash_bytes{
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U, 0x09U, 0x0AU,
        0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U,
        0x16U, 0x17U, 0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU};
    const nova::crypto::Hash256 hash{hash_bytes};

    BinaryWriter writer;
    ASSERT_TRUE(writer.WriteFixedBytes(fixed));
    ASSERT_TRUE(writer.WriteBytes(bytes, 3U));
    ASSERT_TRUE(writer.WriteString("nova", 4U));
    ASSERT_TRUE(writer.WriteArray<std::uint16_t>(
        values, 2U, [](BinaryWriter& array_writer, const std::uint16_t value) {
            return array_writer.WriteU16(value);
        }));
    ASSERT_TRUE(writer.WriteHash256(hash));

    BinaryReader reader{writer.bytes()};
    const auto read_fixed = reader.ReadFixedBytes<3>();
    const auto read_bytes = reader.ReadBytes(3U);
    const auto read_string = reader.ReadString(4U);
    const auto read_values = reader.ReadArray<std::uint16_t>(
        2U, 2U, [](BinaryReader& array_reader) { return array_reader.ReadU16(); });
    const auto read_hash = reader.ReadHash256();
    ASSERT_TRUE(read_fixed.has_value());
    ASSERT_TRUE(read_bytes.has_value());
    ASSERT_TRUE(read_string.has_value());
    ASSERT_TRUE(read_values.has_value());
    ASSERT_TRUE(read_hash.has_value());
    EXPECT_EQ(*read_fixed, fixed);
    EXPECT_EQ(*read_bytes, bytes);
    EXPECT_EQ(*read_string, "nova");
    EXPECT_EQ(*read_values, values);
    EXPECT_EQ(*read_hash, hash);
    EXPECT_TRUE(reader.RequireEnd());
}

TEST(SerializationCompactSize, UsesShortestCanonicalEncoding)
{
    BinaryWriter writer;
    ASSERT_TRUE(writer.WriteCompactSize(252U));
    ASSERT_TRUE(writer.WriteCompactSize(253U));
    ASSERT_TRUE(writer.WriteCompactSize(65'536U));

    const std::vector<std::uint8_t> expected{0xFCU, 0xFDU, 0xFDU, 0x00U, 0xFEU,
                                             0x00U, 0x00U, 0x01U, 0x00U};
    EXPECT_TRUE(
        std::equal(writer.bytes().begin(), writer.bytes().end(), expected.begin(), expected.end()));

    BinaryReader reader{writer.bytes()};
    const auto first = reader.ReadCompactSize(65'536U);
    const auto second = reader.ReadCompactSize(65'536U);
    const auto third = reader.ReadCompactSize(65'536U);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(*first, 252U);
    EXPECT_EQ(*second, 253U);
    EXPECT_EQ(*third, 65'536U);
    EXPECT_TRUE(reader.RequireEnd());
}

TEST(SerializationMalformed, RejectsTruncationNoncanonicalLengthsBoundsAndTrailingBytes)
{
    const std::array<std::uint8_t, 2> truncated_scalar_bytes{0x01U, 0x02U};
    BinaryReader truncated_scalar{truncated_scalar_bytes};
    EXPECT_FALSE(truncated_scalar.ReadU32().has_value());
    EXPECT_EQ(truncated_scalar.error(), DeserializationError::kTruncated);

    const std::array<std::uint8_t, 3> noncanonical_length_bytes{0xFDU, 0xFCU, 0x00U};
    BinaryReader noncanonical_length{noncanonical_length_bytes};
    EXPECT_FALSE(noncanonical_length.ReadCompactSize(1'000U).has_value());
    EXPECT_EQ(noncanonical_length.error(), DeserializationError::kNonCanonicalCompactSize);

    const std::array<std::uint8_t, 4> bounded_bytes_input{0x03U, 0x01U, 0x02U, 0x03U};
    BinaryReader bounded_bytes{bounded_bytes_input};
    EXPECT_FALSE(bounded_bytes.ReadBytes(2U).has_value());
    EXPECT_EQ(bounded_bytes.error(), DeserializationError::kLimitExceeded);

    const std::array<std::uint8_t, 3> impossible_bytes_input{0x03U, 0x01U, 0x02U};
    BinaryReader impossible_bytes{impossible_bytes_input};
    EXPECT_FALSE(impossible_bytes.ReadBytes(3U).has_value());
    EXPECT_EQ(impossible_bytes.error(), DeserializationError::kTruncated);

    const std::array<std::uint8_t, 3> impossible_array_input{0x03U, 0x01U, 0x00U};
    BinaryReader impossible_array{impossible_array_input};
    EXPECT_FALSE(impossible_array
                     .ReadArray<std::uint16_t>(
                         3U, 2U, [](BinaryReader& array_reader) { return array_reader.ReadU16(); })
                     .has_value());
    EXPECT_EQ(impossible_array.error(), DeserializationError::kTruncated);

    const std::array<std::uint8_t, 2> trailing_bytes_input{0x01U, 0x02U};
    BinaryReader trailing_bytes{trailing_bytes_input};
    ASSERT_TRUE(trailing_bytes.ReadU8().has_value());
    EXPECT_FALSE(trailing_bytes.RequireEnd());
    EXPECT_EQ(trailing_bytes.error(), DeserializationError::kTrailingBytes);
}

} // namespace
