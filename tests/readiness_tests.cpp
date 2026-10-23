#include <gtest/gtest.h>

#include "readiness_fixture.hpp"

#include <limits>

namespace
{

TEST(MainnetReadiness, ReorganizationsCheckEveryActiveCoinAtDepthsOneTenAndHundred)
{
    for (const auto depth : {1U, 10U, 100U}) {
        const auto fingerprint = nova::readiness::Reorganize(depth);
        ASSERT_TRUE(fingerprint.has_value()) << "depth=" << depth;
        EXPECT_EQ(fingerprint->height, depth + 1U);
        EXPECT_EQ(fingerprint->coins, depth + 1U);
        EXPECT_EQ(fingerprint, nova::readiness::Reorganize(depth));
    }
    EXPECT_FALSE(nova::readiness::Reorganize(0U).has_value());
    EXPECT_FALSE(nova::readiness::Reorganize(101U).has_value());
}

TEST(MainnetReadiness, IssuanceChecksRejectOverflowAndOneUnitExcessReward)
{
    auto parameters = nova::consensus::RegtestNetworkParams().monetary;
    parameters.halving_interval = std::numeric_limits<std::uint32_t>::max();
    EXPECT_EQ(nova::consensus::CheckChainParams(parameters),
              nova::consensus::ChainParamsError::kTheoreticalIssuanceOverflow);
    EXPECT_FALSE(nova::consensus::CalculateTheoreticalIssuance(parameters).has_value());
    parameters = nova::consensus::TestnetNetworkParams().monetary;
    const auto total = nova::consensus::CalculateTheoreticalIssuance(parameters);
    ASSERT_TRUE(total.has_value());
    EXPECT_LE(*total, nova::consensus::MAX_MONEY - nova::consensus::TESTNET_CREATOR_ALLOCATION);
    const auto subsidy = nova::consensus::GetBlockSubsidy(1U, parameters);
    ASSERT_TRUE(subsidy.has_value());
    const auto block = nova::readiness::Block({}, 1U, 0U);
    ASSERT_TRUE(block.has_value());
    auto coinbase = block->transactions.front();
    EXPECT_EQ(nova::consensus::CheckCoinbaseReward(coinbase, 0, 1U, parameters),
              nova::consensus::CoinbaseRewardError::kNone);
    coinbase.outputs.front().value += 1;
    EXPECT_EQ(nova::consensus::CheckCoinbaseReward(coinbase, 0, 1U, parameters),
              nova::consensus::CoinbaseRewardError::kInflation);
    coinbase.outputs.front().value = std::numeric_limits<nova::primitives::Amount>::max();
    EXPECT_EQ(nova::consensus::CheckCoinbaseReward(coinbase, 0, 1U, parameters),
              nova::consensus::CoinbaseRewardError::kInvalidOutputAmount);
    EXPECT_EQ(
        nova::consensus::CheckCoinbaseReward(coinbase, nova::consensus::MAX_MONEY, 1U, parameters),
        nova::consensus::CoinbaseRewardError::kRewardLimitExceedsMaxMoney);
}

TEST(MainnetConsensusBoundary, EqualWorkTieDoesNotDependOnArrivalOrder)
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto target = nova::consensus::TargetFromCompact(network.pow.pow_limit_compact);
    ASSERT_TRUE(target.target.has_value());
    const auto work = nova::consensus::CalculateWork(*target.target);
    ASSERT_TRUE(work.has_value());
    const auto a = nova::readiness::Block(network.genesis_hash, 1U, 0U);
    const auto b = nova::readiness::Block(network.genesis_hash, 1U, 1U);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    const auto a_hash = nova::primitives::ComputeBlockHash(a->header);
    const auto b_hash = nova::primitives::ComputeBlockHash(b->header);
    ASSERT_TRUE(a_hash.has_value());
    ASSERT_TRUE(b_hash.has_value());
    const auto expected = a_hash->bytes() < b_hash->bytes() ? *a_hash : *b_hash;
    for (const auto reverse : {false, true}) {
        nova::chain::UTXOSet utxos;
        const auto state =
            nova::chain::ChainState::Create(nova::readiness::Parameters(),
                                            {network.genesis_hash,
                                             {},
                                             0U,
                                             1'000U,
                                             *target.target,
                                             *work,
                                             nova::chain::ChainWork::FromPerBlockWork(*work)},
                                            utxos);
        ASSERT_NE(state, nullptr);
        ASSERT_EQ(state->AcceptBlock(reverse ? *b : *a).error, nova::chain::ChainError::kNone);
        ASSERT_EQ(state->AcceptBlock(reverse ? *a : *b).error, nova::chain::ChainError::kNone);
        EXPECT_EQ(state->active_tip(), expected);
        EXPECT_TRUE(nova::readiness::Snapshot(*state, utxos).has_value());
    }
}

TEST(MainnetConsensusBoundary, ChainworkOverflowLeavesTheAccumulatorUnchanged)
{
    const auto target = nova::consensus::TargetFromCompact(0x207F'FFFFU);
    ASSERT_TRUE(target.target.has_value());
    const auto work = nova::consensus::CalculateWork(*target.target);
    ASSERT_TRUE(work.has_value());
    nova::chain::ChainWork::Bytes bytes{};
    bytes.fill(0xFFU);
    nova::chain::ChainWork maximum{bytes};
    EXPECT_FALSE(maximum.Add(*work));
    EXPECT_EQ(maximum.bytes(), bytes);
    auto valid = nova::chain::ChainWork::FromPerBlockWork(*work);
    const auto before = valid;
    EXPECT_TRUE(valid.Add(*work));
    EXPECT_TRUE(before < valid);
}

TEST(MainnetConsensusBoundary, RetargetLowerClampAndPowLimitUseValidatedAncestors)
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto limit = nova::consensus::TargetFromCompact(network.pow.pow_limit_compact);
    ASSERT_TRUE(limit.target.has_value());
    const auto work = nova::consensus::CalculateWork(*limit.target);
    ASSERT_TRUE(work.has_value());
    for (const auto elapsed : {300U, 600U, 2'400U, 12'000U}) {
        nova::chain::UTXOSet utxos;
        auto parameters = nova::readiness::Parameters();
        parameters.difficulty = {600U, 4U, 2'400U, false, false};
        const auto state =
            nova::chain::ChainState::Create(parameters,
                                            {network.genesis_hash,
                                             {},
                                             0U,
                                             1'000U,
                                             *limit.target,
                                             *work,
                                             nova::chain::ChainWork::FromPerBlockWork(*work)},
                                            utxos);
        ASSERT_NE(state, nullptr);
        auto parent = network.genesis_hash;
        for (std::uint32_t height = 1U; height <= 4U; ++height) {
            auto block = nova::readiness::Block(parent, height, 0U);
            ASSERT_TRUE(block.has_value());
            block->header.time = height < 4U ? 1'000U + elapsed * height / 3U : 1'001U + elapsed;
            const auto expected = height == 4U && elapsed <= 600U ? 0x201F'FFFFU : 0x207F'FFFFU;
            block->header.bits = expected;
            const auto target = nova::consensus::TargetFromCompact(expected);
            ASSERT_TRUE(target.target.has_value());
            bool mined{};
            for (std::uint32_t nonce = 0U; nonce < 4'096U; ++nonce) {
                block->header.nonce = nonce;
                const auto hash = nova::primitives::ComputeBlockHash(block->header);
                ASSERT_TRUE(hash.has_value());
                if (nova::consensus::CheckProofOfWork(*hash, *target.target)) {
                    mined = true;
                    break;
                }
            }
            ASSERT_TRUE(mined);
            if (height == 4U) {
                auto wrong = *block;
                wrong.header.bits = expected == 0x201F'FFFFU ? 0x207F'FFFFU : 0x201F'FFFFU;
                const auto rejected = state->AcceptBlock(wrong);
                EXPECT_EQ(rejected.validation_error,
                          nova::chain::BlockValidationError::kUnexpectedBits);
                EXPECT_EQ(state->active_tip(), parent);
            }
            ASSERT_EQ(state->AcceptBlock(*block).error, nova::chain::ChainError::kNone)
                << "elapsed=" << elapsed << " height=" << height;
            parent = *nova::primitives::ComputeBlockHash(block->header);
        }
    }
}

} // namespace
