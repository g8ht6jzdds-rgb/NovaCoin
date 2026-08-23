#include <gtest/gtest.h>

#include "crypto/crypto.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace
{

using nova::crypto::Hash256;

Hash256 HashFromHex(const std::string_view hex)
{
    const auto hash = Hash256::FromHex(hex);
    EXPECT_TRUE(hash.has_value());
    return hash.value_or(Hash256{});
}

std::array<std::uint8_t, 32> Bytes32FromHex(const std::string_view hex)
{
    const auto hash = Hash256::FromHex(hex);
    EXPECT_TRUE(hash.has_value());
    return hash.value_or(Hash256{}).bytes();
}

TEST(CryptoHash, Sha256Vectors)
{
    const std::vector<std::uint8_t> empty{};
    const std::vector<std::uint8_t> abc{'a', 'b', 'c'};

    const auto empty_hash = Hash256::Sha256(empty);
    const auto abc_hash = Hash256::Sha256(abc);
    ASSERT_TRUE(empty_hash.has_value());
    ASSERT_TRUE(abc_hash.has_value());

    EXPECT_EQ(empty_hash->bytes(),
              Bytes32FromHex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    EXPECT_EQ(abc_hash->bytes(),
              Bytes32FromHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST(CryptoHash, DoubleSha256Vector)
{
    const std::vector<std::uint8_t> empty{};
    const auto hash = Hash256::DoubleSha256(empty);
    ASSERT_TRUE(hash.has_value());
    EXPECT_EQ(hash->bytes(),
              Bytes32FromHex("5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456"));
}

TEST(CryptoKeys, DeterministicPublicKeyVector)
{
    const auto private_key = nova::crypto::PrivateKey::FromBytes(
        Bytes32FromHex("0000000000000000000000000000000000000000000000000000000000000001"));
    ASSERT_TRUE(private_key.has_value());

    const auto public_key = private_key->DerivePublicKey();
    ASSERT_TRUE(public_key.has_value());
    const std::array<std::uint8_t, 33> expected{
        0x02U, 0x79U, 0xBEU, 0x66U, 0x7EU, 0xF9U, 0xDCU, 0xBBU, 0xACU, 0x55U, 0xA0U,
        0x62U, 0x95U, 0xCEU, 0x87U, 0x0BU, 0x07U, 0x02U, 0x9BU, 0xFCU, 0xDBU, 0x2DU,
        0xCEU, 0x28U, 0xD9U, 0x59U, 0xF2U, 0x81U, 0x5BU, 0x16U, 0xF8U, 0x17U, 0x98U};
    EXPECT_EQ(public_key->SerializeCompressed(), expected);
}

TEST(CryptoKeys, GenerationProducesUsableKeyPairs)
{
    const auto key_pair = nova::crypto::KeyPair::Generate();
    ASSERT_TRUE(key_pair.has_value());
    const auto derived = key_pair->private_key().DerivePublicKey();
    ASSERT_TRUE(derived.has_value());
    EXPECT_EQ(*derived, key_pair->public_key());
}

TEST(CryptoSignatures, DeterministicSignAndVerify)
{
    const auto private_key = nova::crypto::PrivateKey::FromBytes(
        Bytes32FromHex("0000000000000000000000000000000000000000000000000000000000000001"));
    ASSERT_TRUE(private_key.has_value());
    const auto public_key = private_key->DerivePublicKey();
    ASSERT_TRUE(public_key.has_value());
    const auto digest =
        HashFromHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const auto first = private_key->Sign(digest);
    const auto second = private_key->Sign(digest);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
    EXPECT_TRUE(public_key->Verify(digest, *first));
}

TEST(CryptoSignatures, RejectsWrongKeyAndModifiedMessage)
{
    const auto signing_key = nova::crypto::PrivateKey::FromBytes(
        Bytes32FromHex("0000000000000000000000000000000000000000000000000000000000000001"));
    const auto wrong_key = nova::crypto::PrivateKey::FromBytes(
        Bytes32FromHex("0000000000000000000000000000000000000000000000000000000000000002"));
    ASSERT_TRUE(signing_key.has_value());
    ASSERT_TRUE(wrong_key.has_value());
    const auto signing_public_key = signing_key->DerivePublicKey();
    const auto wrong_public_key = wrong_key->DerivePublicKey();
    ASSERT_TRUE(signing_public_key.has_value());
    ASSERT_TRUE(wrong_public_key.has_value());
    const auto digest =
        HashFromHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const auto changed_digest =
        HashFromHex("bb7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const auto signature = signing_key->Sign(digest);
    ASSERT_TRUE(signature.has_value());

    EXPECT_FALSE(wrong_public_key->Verify(digest, *signature));
    EXPECT_FALSE(signing_public_key->Verify(changed_digest, *signature));
}

TEST(CryptoParsing, RejectsMalformedKeysAndSignatures)
{
    const std::array<std::uint8_t, 32> too_short_key{};
    const std::array<std::uint8_t, 33> invalid_public_key{};
    const std::array<std::uint8_t, 33> invalid_prefix{
        0x04U, 0x79U, 0xBEU, 0x66U, 0x7EU, 0xF9U, 0xDCU, 0xBBU, 0xACU, 0x55U, 0xA0U,
        0x62U, 0x95U, 0xCEU, 0x87U, 0x0BU, 0x07U, 0x02U, 0x9BU, 0xFCU, 0xDBU, 0x2DU,
        0xCEU, 0x28U, 0xD9U, 0x59U, 0xF2U, 0x81U, 0x5BU, 0x16U, 0xF8U, 0x17U, 0x98U};
    const std::array<std::uint8_t, 33> invalid_point{
        0x02U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    const std::array<std::uint8_t, 8> zero_r_signature{0x30U, 0x06U, 0x02U, 0x01U,
                                                       0x00U, 0x02U, 0x01U, 0x01U};
    const std::array<std::uint8_t, 9> noncanonical_signature{0x30U, 0x07U, 0x02U, 0x02U, 0x00U,
                                                             0x01U, 0x02U, 0x01U, 0x01U};
    const std::array<std::uint8_t, 40> high_s_signature{
        0x30U, 0x26U, 0x02U, 0x01U, 0x01U, 0x02U, 0x21U, 0x00U, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFEU, 0xBAU, 0xAEU, 0xDCU, 0xE6U, 0xAFU, 0x48U,
        0xA0U, 0x3BU, 0xBFU, 0xD2U, 0x5EU, 0x8CU, 0xD0U, 0x36U, 0x41U, 0x40U};
    const auto curve_order =
        Bytes32FromHex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");

    EXPECT_FALSE(
        nova::crypto::PrivateKey::FromBytes(std::span<const std::uint8_t>{too_short_key}.first(31U))
            .has_value());
    EXPECT_FALSE(nova::crypto::PrivateKey::FromBytes(too_short_key).has_value());
    EXPECT_FALSE(nova::crypto::PrivateKey::FromBytes(curve_order).has_value());
    EXPECT_FALSE(nova::crypto::PublicKey::FromCompressed(invalid_public_key).has_value());
    EXPECT_FALSE(nova::crypto::PublicKey::FromCompressed(invalid_prefix).has_value());
    EXPECT_FALSE(nova::crypto::PublicKey::FromCompressed(invalid_point).has_value());
    EXPECT_FALSE(nova::crypto::Signature::FromDer(std::span<const std::uint8_t>{}).has_value());
    EXPECT_FALSE(nova::crypto::Signature::FromDer(zero_r_signature).has_value());
    EXPECT_FALSE(nova::crypto::Signature::FromDer(noncanonical_signature).has_value());
    EXPECT_FALSE(nova::crypto::Signature::FromDer(high_s_signature).has_value());
}

} // namespace
