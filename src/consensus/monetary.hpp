#pragma once

#include "primitives/transaction.hpp"

#include <cstdint>
#include <optional>

namespace nova::consensus
{

using primitives::Amount;

inline constexpr Amount COIN = 100'000'000LL;
inline constexpr Amount MAX_MONEY = 21'000'000LL * COIN;
inline constexpr Amount INITIAL_SUBSIDY = 50LL * COIN;

// Monetary constants are carried with the network's chain parameters so a
// validator never relies on an implicit process-wide default.
struct ChainParams final {
    Amount coin{COIN};
    Amount max_money{MAX_MONEY};
    Amount initial_subsidy{INITIAL_SUBSIDY};
    std::uint32_t halving_interval{};
};

enum class ChainParamsError : std::uint8_t {
    kNone,
    kInvalidMonetaryConstants,
    kZeroHalvingInterval,
    kTheoreticalIssuanceOverflow,
    kTheoreticalIssuanceExceedsMaxMoney,
};

enum class CoinbaseRewardError : std::uint8_t {
    kNone,
    kInvalidChainParams,
    kNotCoinbase,
    kNoOutputs,
    kInvalidTransactionFees,
    kInvalidOutputAmount,
    kCoinbaseOutputTotalExceedsMaxMoney,
    kRewardLimitExceedsMaxMoney,
    kInflation,
};

[[nodiscard]] ChainParamsError CheckChainParams(const ChainParams& parameters) noexcept;
[[nodiscard]] std::optional<Amount> GetBlockSubsidy(std::uint32_t height,
                                                    const ChainParams& parameters) noexcept;
[[nodiscard]] std::optional<Amount>
CalculateTheoreticalIssuance(const ChainParams& parameters) noexcept;
[[nodiscard]] CoinbaseRewardError CheckCoinbaseReward(const primitives::Transaction& coinbase,
                                                      Amount transaction_fees, std::uint32_t height,
                                                      const ChainParams& parameters) noexcept;

} // namespace nova::consensus
