#include <gtest/gtest.h>

#include "consensus/monetary.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace
{

using nova::consensus::CalculateTheoreticalIssuance;
using nova::consensus::ChainParams;
using nova::consensus::ChainParamsError;
using nova::consensus::CheckChainParams;
using nova::consensus::CheckCoinbaseReward;
using nova::consensus::COIN;
using nova::consensus::CoinbaseRewardError;
using nova::consensus::GetBlockSubsidy;
using nova::consensus::INITIAL_SUBSIDY;
using nova::consensus::MAX_MONEY;
using nova::primitives::Amount;
using nova::primitives::OutPoint;
using nova::primitives::Transaction;
using nova::primitives::TxInput;
using nova::primitives::TxOutput;

constexpr ChainParams kShortIntervalParams{COIN, MAX_MONEY, INITIAL_SUBSIDY, 10U};
constexpr ChainParams kMaximumIssuanceParams{COIN, MAX_MONEY, INITIAL_SUBSIDY, 210'000U};

Transaction CoinbaseWithOutputs(std::vector<TxOutput> outputs)
{
    return Transaction{1,
                       std::vector<TxInput>{TxInput{
                           OutPoint{nova::crypto::Hash256{}, 0xFFFF'FFFFU}, {0x01U, 0x00U}, 0U}},
                       std::move(outputs), 0U};
}

TEST(MonetarySubsidy, HasExactIntegerValuesAcrossHalvingBoundaries)
{
    const auto at_zero = GetBlockSubsidy(0U, kShortIntervalParams);
    const auto at_one = GetBlockSubsidy(1U, kShortIntervalParams);
    const auto before_first_halving = GetBlockSubsidy(9U, kShortIntervalParams);
    const auto at_first_halving = GetBlockSubsidy(10U, kShortIntervalParams);
    const auto after_first_halving = GetBlockSubsidy(11U, kShortIntervalParams);
    const auto after_multiple_halvings = GetBlockSubsidy(20U, kShortIntervalParams);
    ASSERT_TRUE(at_zero.has_value());
    ASSERT_TRUE(at_one.has_value());
    ASSERT_TRUE(before_first_halving.has_value());
    ASSERT_TRUE(at_first_halving.has_value());
    ASSERT_TRUE(after_first_halving.has_value());
    ASSERT_TRUE(after_multiple_halvings.has_value());
    EXPECT_EQ(*at_zero, 50LL * COIN);
    EXPECT_EQ(*at_one, 50LL * COIN);
    EXPECT_EQ(*before_first_halving, 50LL * COIN);
    EXPECT_EQ(*at_first_halving, 25LL * COIN);
    EXPECT_EQ(*after_first_halving, 25LL * COIN);
    EXPECT_EQ(*after_multiple_halvings, 12LL * COIN + 50'000'000LL);

    const auto final_era = GetBlockSubsidy(320U, kShortIntervalParams);
    const auto post_subsidy = GetBlockSubsidy(330U, kShortIntervalParams);
    ASSERT_TRUE(final_era.has_value());
    ASSERT_TRUE(post_subsidy.has_value());
    EXPECT_EQ(*final_era, 1LL);
    EXPECT_EQ(*post_subsidy, 0LL);
}

TEST(MonetaryParameters, RejectsInvalidIntervalsConstantsAndExcessIssuance)
{
    auto zero_interval = kShortIntervalParams;
    zero_interval.halving_interval = 0U;
    EXPECT_EQ(CheckChainParams(zero_interval), ChainParamsError::kZeroHalvingInterval);
    EXPECT_FALSE(GetBlockSubsidy(0U, zero_interval).has_value());

    auto altered_money = kShortIntervalParams;
    altered_money.max_money -= 1;
    EXPECT_EQ(CheckChainParams(altered_money), ChainParamsError::kInvalidMonetaryConstants);

    const ChainParams excessive_issuance{COIN, MAX_MONEY, INITIAL_SUBSIDY, 210'001U};
    EXPECT_EQ(CheckChainParams(excessive_issuance),
              ChainParamsError::kTheoreticalIssuanceExceedsMaxMoney);
    EXPECT_FALSE(CalculateTheoreticalIssuance(excessive_issuance).has_value());
}

TEST(MonetaryIssuance, SummingEveryTheoreticalBlockSubsidyNeverExceedsMaxMoney)
{
    Amount total = 0;
    bool reached_post_subsidy_height = false;
    for (std::uint32_t height = 0U; height < 340U; ++height) {
        const auto subsidy = GetBlockSubsidy(height, kShortIntervalParams);
        ASSERT_TRUE(subsidy.has_value());
        if (*subsidy == 0) {
            reached_post_subsidy_height = true;
            break;
        }
        ASSERT_LE(*subsidy, MAX_MONEY - total);
        total += *subsidy;
    }
    ASSERT_TRUE(reached_post_subsidy_height);
    const auto theoretical_total = CalculateTheoreticalIssuance(kShortIntervalParams);
    ASSERT_TRUE(theoretical_total.has_value());
    EXPECT_EQ(total, *theoretical_total);
    EXPECT_LE(total, MAX_MONEY);

    const auto maximum_interval_total = CalculateTheoreticalIssuance(kMaximumIssuanceParams);
    ASSERT_TRUE(maximum_interval_total.has_value());
    EXPECT_LE(*maximum_interval_total, MAX_MONEY);
}

TEST(MonetaryCoinbaseReward, PermitsAtMostSubsidyPlusValidatedFees)
{
    constexpr Amount kFees = 125LL;
    const auto full_reward = CoinbaseWithOutputs({TxOutput{INITIAL_SUBSIDY + kFees, {0x51U}}});
    EXPECT_EQ(CheckCoinbaseReward(full_reward, kFees, 0U, kShortIntervalParams),
              CoinbaseRewardError::kNone);

    const auto unclaimed_reward = CoinbaseWithOutputs({TxOutput{INITIAL_SUBSIDY, {0x51U}}});
    EXPECT_EQ(CheckCoinbaseReward(unclaimed_reward, kFees, 0U, kShortIntervalParams),
              CoinbaseRewardError::kNone);
}

TEST(MonetaryCoinbaseReward, RejectsInflationAndMalformedMonetaryInputs)
{
    constexpr Amount kFees = 125LL;
    const auto inflation = CoinbaseWithOutputs({TxOutput{INITIAL_SUBSIDY + kFees + 1LL, {0x51U}}});
    EXPECT_EQ(CheckCoinbaseReward(inflation, kFees, 0U, kShortIntervalParams),
              CoinbaseRewardError::kInflation);

    const auto negative_output = CoinbaseWithOutputs({TxOutput{-1LL, {0x51U}}});
    EXPECT_EQ(CheckCoinbaseReward(negative_output, 0LL, 0U, kShortIntervalParams),
              CoinbaseRewardError::kInvalidOutputAmount);

    const auto oversized_total =
        CoinbaseWithOutputs({TxOutput{MAX_MONEY, {0x51U}}, TxOutput{1LL, {0x51U}}});
    EXPECT_EQ(CheckCoinbaseReward(oversized_total, 0LL, 0U, kShortIntervalParams),
              CoinbaseRewardError::kCoinbaseOutputTotalExceedsMaxMoney);

    const auto valid_coinbase = CoinbaseWithOutputs({TxOutput{INITIAL_SUBSIDY, {0x51U}}});
    EXPECT_EQ(CheckCoinbaseReward(valid_coinbase, -1LL, 0U, kShortIntervalParams),
              CoinbaseRewardError::kInvalidTransactionFees);

    auto non_coinbase = valid_coinbase;
    non_coinbase.inputs.front().previous_output.output_index = 0U;
    EXPECT_EQ(CheckCoinbaseReward(non_coinbase, 0LL, 0U, kShortIntervalParams),
              CoinbaseRewardError::kNotCoinbase);
}

} // namespace
