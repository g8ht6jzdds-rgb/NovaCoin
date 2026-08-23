#include <gtest/gtest.h>

#include "consensus/pow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace
{

using nova::consensus::CalculateWork;
using nova::consensus::CheckProofOfWork;
using nova::consensus::CompactFromTarget;
using nova::consensus::MineRegtestBlock;
using nova::consensus::RegtestPowParameters;
using nova::consensus::Target256;
using nova::consensus::TargetError;
using nova::consensus::TargetFromCompact;
using nova::consensus::ValidateTarget;

Target256 TargetFromHex(const char* hex)
{
    const auto target = Target256::FromHex(hex);
    EXPECT_TRUE(target.has_value());
    return target.value_or(Target256{});
}

nova::crypto::Hash256 HashFromHex(const char* hex)
{
    const auto hash = nova::crypto::Hash256::FromHex(hex);
    EXPECT_TRUE(hash.has_value());
    return hash.value_or(nova::crypto::Hash256{});
}

nova::crypto::Hash256 HashForTarget(const Target256& target)
{
    nova::crypto::Hash256::Bytes hash_bytes{};
    std::reverse_copy(target.bytes().begin(), target.bytes().end(), hash_bytes.begin());
    return nova::crypto::Hash256{hash_bytes};
}

TEST(PowCompactTarget, DecodesAndEncodesCanonicalTargets)
{
    const auto decoded = TargetFromCompact(0x1D00'FFFFU);
    ASSERT_EQ(decoded.error, TargetError::kNone);
    ASSERT_TRUE(decoded.target.has_value());
    EXPECT_EQ(*decoded.target,
              TargetFromHex("00000000ffff0000000000000000000000000000000000000000000000000000"));

    const auto encoded = CompactFromTarget(*decoded.target);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(*encoded, 0x1D00'FFFFU);

    const auto regtest = TargetFromCompact(RegtestPowParameters().pow_limit_compact);
    ASSERT_EQ(regtest.error, TargetError::kNone);
    ASSERT_TRUE(regtest.target.has_value());
    EXPECT_EQ(*regtest.target, RegtestPowParameters().pow_limit);
}

TEST(PowCompactTarget, RejectsMalformedAndNonCanonicalEncodings)
{
    EXPECT_EQ(TargetFromCompact(0x1D80'FFFFU).error, TargetError::kNegative);
    EXPECT_EQ(TargetFromCompact(0x1D00'0000U).error, TargetError::kZero);
    EXPECT_EQ(TargetFromCompact(0x2300'0001U).error, TargetError::kOverflow);
    EXPECT_EQ(TargetFromCompact(0x2200'0100U).error, TargetError::kOverflow);
    EXPECT_EQ(TargetFromCompact(0x0400'0100U).error, TargetError::kNonCanonical);
}

TEST(PowValidation, RejectsZeroAndTargetsAboveTheConfiguredLimit)
{
    const Target256 zero{};
    EXPECT_EQ(ValidateTarget(zero, RegtestPowParameters().pow_limit), TargetError::kZero);

    const auto mainnet_style_limit = TargetFromCompact(0x1D00'FFFFU);
    const auto above_limit = TargetFromCompact(0x1D01'0000U);
    ASSERT_TRUE(mainnet_style_limit.target.has_value());
    ASSERT_TRUE(above_limit.target.has_value());
    EXPECT_EQ(ValidateTarget(*above_limit.target, *mainnet_style_limit.target),
              TargetError::kAbovePowLimit);
    const nova::consensus::PowParameters parameters{*mainnet_style_limit.target, 0x1D00'FFFFU};
    EXPECT_EQ(TargetFromCompact(0x1D01'0000U, parameters).error, TargetError::kAbovePowLimit);
}

TEST(PowValidation, UsesAnInclusiveLittleEndianHashComparison)
{
    const auto target =
        TargetFromHex("0000000000000000000000000000000000000000000000000000000000000100");
    const auto equal_hash = HashForTarget(target);
    EXPECT_TRUE(CheckProofOfWork(equal_hash, target));

    const auto lower_target =
        TargetFromHex("00000000000000000000000000000000000000000000000000000000000000ff");
    const auto lower_hash = HashForTarget(lower_target);
    EXPECT_TRUE(CheckProofOfWork(lower_hash, target));

    const auto higher_target =
        TargetFromHex("0000000000000000000000000000000000000000000000000000000000000101");
    const auto higher_hash = HashForTarget(higher_target);
    EXPECT_FALSE(CheckProofOfWork(higher_hash, target));
    EXPECT_FALSE(CheckProofOfWork(equal_hash, Target256{}));
}

TEST(PowWork, CalculatesExactWorkWithoutFloatingPoint)
{
    const auto one =
        TargetFromHex("0000000000000000000000000000000000000000000000000000000000000001");
    const auto work_for_one = CalculateWork(one);
    ASSERT_TRUE(work_for_one.has_value());
    EXPECT_EQ(*work_for_one,
              TargetFromHex("8000000000000000000000000000000000000000000000000000000000000000"));

    const auto maximum =
        TargetFromHex("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    const auto work_for_maximum = CalculateWork(maximum);
    ASSERT_TRUE(work_for_maximum.has_value());
    EXPECT_EQ(*work_for_maximum,
              TargetFromHex("0000000000000000000000000000000000000000000000000000000000000001"));
    EXPECT_FALSE(CalculateWork(Target256{}).has_value());
}

TEST(PowRegtestMiner, SearchesNonceValuesWithTheExplicitEasyRegtestTarget)
{
    nova::primitives::BlockHeader candidate{};
    candidate.version = 1;
    candidate.time = 1'700'000'000U;
    candidate.nonce = 41U;

    EXPECT_FALSE(MineRegtestBlock(candidate, 0U).has_value());
    const auto result = MineRegtestBlock(candidate, 4'096U);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->header.bits, RegtestPowParameters().pow_limit_compact);
    EXPECT_EQ(result->attempts, 1U);
    EXPECT_EQ(result->header.nonce, 41U);
    EXPECT_EQ(result->block_hash,
              HashFromHex("12bf674640cf1b74b9026756d0cf2d86819330b85672e6709bbbe185a0f9ab7c"));
    EXPECT_TRUE(CheckProofOfWork(result->block_hash, RegtestPowParameters().pow_limit));
}

} // namespace
