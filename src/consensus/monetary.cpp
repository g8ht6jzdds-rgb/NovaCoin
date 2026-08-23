#include "consensus/monetary.hpp"

#include "primitives/block.hpp"

#include <cstdint>
#include <limits>

namespace nova::consensus
{
namespace
{

constexpr std::uint32_t kMaximumSafeSubsidyShift = 63U;

[[nodiscard]] std::optional<Amount>
CalculateIssuanceUnchecked(const ChainParams& parameters) noexcept
{
    Amount total = 0;
    Amount subsidy = parameters.initial_subsidy;
    const auto interval = static_cast<Amount>(parameters.halving_interval);
    while (subsidy != 0) {
        if (subsidy > std::numeric_limits<Amount>::max() / interval) {
            return std::nullopt;
        }
        const auto era_issuance = subsidy * interval;
        if (era_issuance > std::numeric_limits<Amount>::max() - total) {
            return std::nullopt;
        }
        total += era_issuance;
        subsidy /= 2;
    }
    return total;
}

} // namespace

ChainParamsError CheckChainParams(const ChainParams& parameters) noexcept
{
    if (parameters.coin != COIN || parameters.max_money != MAX_MONEY ||
        parameters.initial_subsidy != INITIAL_SUBSIDY) {
        return ChainParamsError::kInvalidMonetaryConstants;
    }
    if (parameters.halving_interval == 0U) {
        return ChainParamsError::kZeroHalvingInterval;
    }
    const auto issuance = CalculateIssuanceUnchecked(parameters);
    if (!issuance.has_value()) {
        return ChainParamsError::kTheoreticalIssuanceOverflow;
    }
    if (*issuance > parameters.max_money) {
        return ChainParamsError::kTheoreticalIssuanceExceedsMaxMoney;
    }
    return ChainParamsError::kNone;
}

std::optional<Amount> GetBlockSubsidy(const std::uint32_t height,
                                      const ChainParams& parameters) noexcept
{
    if (CheckChainParams(parameters) != ChainParamsError::kNone) {
        return std::nullopt;
    }
    const auto halvings = height / parameters.halving_interval;
    if (halvings >= kMaximumSafeSubsidyShift) {
        return Amount{0};
    }
    const auto subsidy = static_cast<std::uint64_t>(parameters.initial_subsidy) >> halvings;
    return static_cast<Amount>(subsidy);
}

std::optional<Amount> CalculateTheoreticalIssuance(const ChainParams& parameters) noexcept
{
    if (CheckChainParams(parameters) != ChainParamsError::kNone) {
        return std::nullopt;
    }
    return CalculateIssuanceUnchecked(parameters);
}

CoinbaseRewardError CheckCoinbaseReward(const primitives::Transaction& coinbase,
                                        const Amount transaction_fees, const std::uint32_t height,
                                        const ChainParams& parameters) noexcept
{
    if (CheckChainParams(parameters) != ChainParamsError::kNone) {
        return CoinbaseRewardError::kInvalidChainParams;
    }
    if (!primitives::IsCoinbaseTransaction(coinbase)) {
        return CoinbaseRewardError::kNotCoinbase;
    }
    if (coinbase.outputs.empty()) {
        return CoinbaseRewardError::kNoOutputs;
    }
    if (transaction_fees < 0 || transaction_fees > parameters.max_money) {
        return CoinbaseRewardError::kInvalidTransactionFees;
    }

    const auto subsidy = GetBlockSubsidy(height, parameters);
    if (!subsidy.has_value()) {
        return CoinbaseRewardError::kInvalidChainParams;
    }
    if (transaction_fees > parameters.max_money - *subsidy) {
        return CoinbaseRewardError::kRewardLimitExceedsMaxMoney;
    }
    const auto reward_limit = *subsidy + transaction_fees;

    Amount output_total = 0;
    for (const auto& output : coinbase.outputs) {
        if (output.value < 0 || output.value > parameters.max_money) {
            return CoinbaseRewardError::kInvalidOutputAmount;
        }
        if (output.value > parameters.max_money - output_total) {
            return CoinbaseRewardError::kCoinbaseOutputTotalExceedsMaxMoney;
        }
        output_total += output.value;
    }
    if (output_total > reward_limit) {
        return CoinbaseRewardError::kInflation;
    }
    return CoinbaseRewardError::kNone;
}

} // namespace nova::consensus
