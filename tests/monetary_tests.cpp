#include <gtest/gtest.h>

#include "consensus/monetary.hpp"

#include "consensus/network_params.hpp"
#include <cstdint>
#include <limits>
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
using nova::consensus::TESTNET_CREATOR_ALLOCATION;
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

    const ChainParams excessive_issuance{COIN, MAX_MONEY, INITIAL_SUBSIDY, 310'001U};
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

TEST(MonetaryIssuance, TestnetCreatorAllocationAndScheduledIssuanceFitTheSupplyCap)
{
    const auto scheduled = CalculateTheoreticalIssuance(kMaximumIssuanceParams);
    ASSERT_TRUE(scheduled.has_value());
    ASSERT_LE(TESTNET_CREATOR_ALLOCATION, MAX_MONEY - *scheduled);
    EXPECT_LE(TESTNET_CREATOR_ALLOCATION + *scheduled, MAX_MONEY);
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

TEST(MonetaryNetworkSchedule, EveryEraAndFinalUnitMatchExactAcceptedNetworkTotals)
{
    for (const auto* network :
         {&nova::consensus::RegtestNetworkParams(), &nova::consensus::TestnetNetworkParams()}) {
        SCOPED_TRACE(network->monetary.halving_interval);
        const auto& parameters = network->monetary;
        const auto interval = parameters.halving_interval;
        ASSERT_GT(interval, 0U);
        Amount reference = INITIAL_SUBSIDY;
        Amount total = 0;
        for (std::uint32_t era = 0U; era < 64U; ++era) {
            ASSERT_LE(era + 1U, std::numeric_limits<std::uint32_t>::max() / interval);
            const auto first = era * interval;
            const auto last = first + interval - 1U;
            EXPECT_EQ(GetBlockSubsidy(first, parameters), reference);
            EXPECT_EQ(GetBlockSubsidy(last, parameters), reference);
            if (first != 0U)
                EXPECT_EQ(GetBlockSubsidy(first - 1U, parameters),
                          era <= 33U ? static_cast<Amount>(INITIAL_SUBSIDY) /
                                           static_cast<Amount>(std::uint64_t{1U} << (era - 1U))
                                     : Amount{0});
            ASSERT_LE(reference, MAX_MONEY / static_cast<Amount>(interval));
            const auto issuance = reference * static_cast<Amount>(interval);
            ASSERT_LE(issuance, MAX_MONEY - total);
            total += issuance;
            reference /= 2;
        }
        EXPECT_EQ(GetBlockSubsidy(32U * interval, parameters), Amount{1});
        EXPECT_EQ(GetBlockSubsidy(33U * interval, parameters), Amount{0});
        EXPECT_EQ(GetBlockSubsidy(std::numeric_limits<std::uint32_t>::max(), parameters),
                  Amount{0});
        const Amount expected = interval == 150U ? 1'499'999'998'350LL : 2'099'999'997'690'000LL;
        EXPECT_EQ(total, expected);
        EXPECT_EQ(CalculateTheoreticalIssuance(parameters), expected);
        const auto genesis = network->genesis_block.transactions.front().outputs.front().value;
        ASSERT_GE(genesis, 0);
        ASSERT_LE(genesis, MAX_MONEY - total);
        EXPECT_LE(total + genesis, MAX_MONEY);
    }
}

TEST(MonetaryOverflowPaths, InvalidIntervalsRejectMultiplicationAdditionAndSupplyExcess)
{
    auto parameters = nova::consensus::TestnetNetworkParams().monetary;
    for (const auto& [interval, expected] :
         {std::pair{0U, ChainParamsError::kZeroHalvingInterval},
          std::pair{310'001U, ChainParamsError::kTheoreticalIssuanceExceedsMaxMoney},
          std::pair{1'000'000'000U, ChainParamsError::kTheoreticalIssuanceOverflow},
          std::pair{2'000'000'000U, ChainParamsError::kTheoreticalIssuanceOverflow},
          std::pair{std::numeric_limits<std::uint32_t>::max(),
                    ChainParamsError::kTheoreticalIssuanceOverflow}}) {
        // Invalid API parameters only; no network table/profile is modified.
        parameters.halving_interval = interval;
        EXPECT_EQ(CheckChainParams(parameters), expected);
        EXPECT_FALSE(CalculateTheoreticalIssuance(parameters).has_value());
        EXPECT_FALSE(GetBlockSubsidy(0U, parameters).has_value());
        EXPECT_EQ(parameters.halving_interval, interval);
    }
    parameters.halving_interval = 310'000U;
    EXPECT_EQ(CheckChainParams(parameters), ChainParamsError::kNone);
    const auto total = CalculateTheoreticalIssuance(parameters);
    ASSERT_TRUE(total.has_value());
    EXPECT_LE(*total, MAX_MONEY);
}

TEST(MonetaryRewardBoundaries, FeesOutputSumsAndZeroSubsidyRejectBeforeOverflow)
{
    const auto& parameters = nova::consensus::TestnetNetworkParams().monetary;
    const auto at_cap = CoinbaseWithOutputs({{MAX_MONEY, {0x51U}}});
    const auto largest_fee = MAX_MONEY - INITIAL_SUBSIDY;
    EXPECT_EQ(CheckCoinbaseReward(at_cap, largest_fee, 0U, parameters), CoinbaseRewardError::kNone);
    EXPECT_EQ(CheckCoinbaseReward(at_cap, largest_fee + 1, 0U, parameters),
              CoinbaseRewardError::kRewardLimitExceedsMaxMoney);
    for (const auto fee : {Amount{-1}, MAX_MONEY + 1, std::numeric_limits<Amount>::min(),
                           std::numeric_limits<Amount>::max()})
        EXPECT_EQ(CheckCoinbaseReward(at_cap, fee, 0U, parameters),
                  CoinbaseRewardError::kInvalidTransactionFees);
    for (const auto value : {Amount{-1}, MAX_MONEY + 1, std::numeric_limits<Amount>::min(),
                             std::numeric_limits<Amount>::max()})
        EXPECT_EQ(CheckCoinbaseReward(CoinbaseWithOutputs({{value, {0x51U}}}), 0, 0U, parameters),
                  CoinbaseRewardError::kInvalidOutputAmount);
    EXPECT_EQ(CheckCoinbaseReward(CoinbaseWithOutputs({{MAX_MONEY, {0x51U}}, {1, {0x51U}}}), 0, 0U,
                                  parameters),
              CoinbaseRewardError::kCoinbaseOutputTotalExceedsMaxMoney);
    const auto zero_height = 33U * parameters.halving_interval;
    EXPECT_EQ(CheckCoinbaseReward(CoinbaseWithOutputs({{0, {0x51U}}}), 0, zero_height, parameters),
              CoinbaseRewardError::kNone);
    EXPECT_EQ(CheckCoinbaseReward(CoinbaseWithOutputs({{1, {0x51U}}}), 0, zero_height, parameters),
              CoinbaseRewardError::kInflation);
    EXPECT_EQ(CheckCoinbaseReward(CoinbaseWithOutputs({{1, {0x51U}}}), 1, zero_height, parameters),
              CoinbaseRewardError::kNone);
    EXPECT_EQ(CheckCoinbaseReward(CoinbaseWithOutputs({{2, {0x51U}}}), 1, zero_height, parameters),
              CoinbaseRewardError::kInflation);
}

} // namespace
