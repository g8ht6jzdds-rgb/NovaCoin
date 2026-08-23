#include <gtest/gtest.h>

#include "chain/chainstate.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace
{

using nova::chain::BlockStatus;
using nova::chain::ChainAnchor;
using nova::chain::ChainError;
using nova::chain::ChainState;
using nova::chain::ChainStateParams;
using nova::chain::ChainWork;
using nova::chain::UTXOKey;
using nova::chain::UTXOSet;
using nova::consensus::ChainParams;
using nova::consensus::COIN;
using nova::consensus::INITIAL_SUBSIDY;
using nova::consensus::MAX_MONEY;
using nova::primitives::Block;
using nova::primitives::BlockLimits;
using nova::primitives::OutPoint;
using nova::primitives::Transaction;
using nova::primitives::TransactionLimits;
using nova::primitives::TxInput;
using nova::primitives::TxOutput;

constexpr std::uint32_t kRegularBits = 0x207F'FFFFU;

nova::crypto::Hash256 HashWithLastByte(const std::uint8_t value)
{
    nova::crypto::Hash256::Bytes bytes{};
    bytes.back() = value;
    return nova::crypto::Hash256{bytes};
}

ChainStateParams TestParameters()
{
    constexpr TransactionLimits kTransactionLimits{MAX_MONEY, 100'000U, 8U, 8U, 128U};
    constexpr BlockLimits kBlockLimits{kTransactionLimits, 100'000U, 8U, 64U};
    return ChainStateParams{{kBlockLimits, nova::consensus::RegtestPowParameters(),
                             ChainParams{COIN, MAX_MONEY, INITIAL_SUBSIDY, 10U}, 0U, 500'000'000U,
                             0xFFFF'FFFFU, 1U, 600U},
                            {600U, 1U, 600U, true, true},
                            3U,
                            std::make_shared<nova::chain::FixedValidationTimeSource>(10'000U)};
}

std::unique_ptr<ChainState> CreateState(UTXOSet& utxos)
{
    const auto target = nova::consensus::TargetFromCompact(kRegularBits);
    if (!target.target.has_value()) {
        return nullptr;
    }
    const auto work = nova::consensus::CalculateWork(*target.target);
    if (!work.has_value()) {
        return nullptr;
    }
    return ChainState::Create(TestParameters(),
                              ChainAnchor{HashWithLastByte(0xA0U), nova::crypto::Hash256{}, 0U,
                                          1'000U, *target.target, *work,
                                          ChainWork::FromPerBlockWork(*work)},
                              utxos);
}

std::optional<Block> MakeCoinbaseBlock(const nova::crypto::Hash256& parent,
                                       const std::uint32_t height, const std::uint32_t time,
                                       const std::uint32_t bits, const ChainStateParams& parameters)
{
    const auto subsidy =
        nova::consensus::GetBlockSubsidy(height, parameters.block_validation.chain_parameters);
    const auto target =
        nova::consensus::TargetFromCompact(bits, parameters.block_validation.pow_parameters);
    if (!subsidy.has_value() || !target.target.has_value()) {
        return std::nullopt;
    }
    Transaction coinbase{
        1,
        std::vector<TxInput>{TxInput{
            OutPoint{nova::crypto::Hash256{}, 0xFFFF'FFFFU},
            {0x01U, static_cast<std::uint8_t>(height), static_cast<std::uint8_t>(time & 0xFFU)},
            0U}},
        std::vector<TxOutput>{TxOutput{*subsidy, {0x51U}}}, 0U};
    Block block{{1, parent, nova::crypto::Hash256{}, time, bits, 0U}, {std::move(coinbase)}};
    const auto root = nova::primitives::ComputeMerkleRoot(
        block.transactions, parameters.block_validation.block_limits.transaction_limits);
    if (!root.has_value()) {
        return std::nullopt;
    }
    block.header.merkle_root = *root;
    for (std::uint32_t attempt = 0U; attempt < 4'096U; ++attempt) {
        block.header.nonce = attempt;
        const auto hash = nova::primitives::ComputeBlockHash(block.header);
        if (hash.has_value() && nova::consensus::CheckProofOfWork(*hash, *target.target)) {
            return block;
        }
    }
    return std::nullopt;
}

nova::crypto::Hash256 BlockHash(const Block& block)
{
    const auto hash = nova::primitives::ComputeBlockHash(block.header);
    EXPECT_TRUE(hash.has_value());
    return hash.value_or(nova::crypto::Hash256{});
}

UTXOKey CoinbaseOutputKey(const Block& block, const ChainStateParams& parameters)
{
    const auto transaction_id = block.transactions.front().TxId(
        parameters.block_validation.block_limits.transaction_limits);
    EXPECT_TRUE(transaction_id.has_value());
    return UTXOKey{transaction_id.value_or(nova::crypto::Hash256{}), 0U};
}

void Accept(ChainState& state, const Block& block, const std::uint32_t validation_time,
            const std::uint32_t expected_bits)
{
    static_cast<void>(validation_time);
    static_cast<void>(expected_bits);
    const auto result = state.AcceptBlock(block);
    ASSERT_EQ(result.error, ChainError::kNone);
}

TEST(ChainStateSelection, ExtendsAStraightChainByCumulativeWork)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    const auto first = MakeCoinbaseBlock(state->active_tip(), 1U, 1'100U, kRegularBits, parameters);
    ASSERT_TRUE(first.has_value());
    Accept(*state, *first, 1'200U, kRegularBits);
    const auto second = MakeCoinbaseBlock(BlockHash(*first), 2U, 1'200U, kRegularBits, parameters);
    ASSERT_TRUE(second.has_value());
    Accept(*state, *second, 1'300U, kRegularBits);

    EXPECT_EQ(state->active_tip(), BlockHash(*second));
    EXPECT_EQ(state->active_height(), 2U);
    EXPECT_EQ(state->known_block_count(), 3U);
    EXPECT_EQ(utxos.size(), 2U);
    const auto index = state->GetBlockIndex(BlockHash(*second));
    ASSERT_TRUE(index.has_value());
    const auto target = nova::consensus::TargetFromCompact(kRegularBits);
    ASSERT_TRUE(target.target.has_value());
    const auto work = nova::consensus::CalculateWork(*target.target);
    ASSERT_TRUE(work.has_value());
    const auto previous = state->GetBlockIndex(BlockHash(*first));
    ASSERT_TRUE(previous.has_value());
    EXPECT_EQ(index->previous_hash, BlockHash(*first));
    EXPECT_EQ(index->height, 2U);
    EXPECT_EQ(index->target, *target.target);
    EXPECT_EQ(index->per_block_work, *work);
    EXPECT_TRUE(previous->chain_work < index->chain_work);
    EXPECT_EQ(index->status, BlockStatus::kActive);
}

TEST(ChainStateSelection, RetainsTheHigherWorkBranchOverALowerWorkSideBranch)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    const auto first = MakeCoinbaseBlock(state->active_tip(), 1U, 1'100U, kRegularBits, parameters);
    ASSERT_TRUE(first.has_value());
    Accept(*state, *first, 1'200U, kRegularBits);
    const auto main = MakeCoinbaseBlock(BlockHash(*first), 2U, 1'200U, kRegularBits, parameters);
    ASSERT_TRUE(main.has_value());
    Accept(*state, *main, 1'300U, kRegularBits);
    const auto side = MakeCoinbaseBlock(BlockHash(*first), 2U, 1'210U, kRegularBits, parameters);
    ASSERT_TRUE(side.has_value());
    Accept(*state, *side, 1'310U, kRegularBits);

    EXPECT_EQ(state->active_tip(), BlockHash(*main));
    const auto side_index = state->GetBlockIndex(BlockHash(*side));
    ASSERT_TRUE(side_index.has_value());
    EXPECT_EQ(side_index->status, BlockStatus::kValid);
    const auto activation = state->ActivateBestChain();
    EXPECT_EQ(activation.error, ChainError::kNone);
    EXPECT_EQ(state->active_tip(), BlockHash(*main));
    EXPECT_EQ(utxos.size(), 2U);
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*main, parameters)));
    EXPECT_FALSE(utxos.HaveCoin(CoinbaseOutputKey(*side, parameters)));
}

TEST(ChainStateSelection, ActivatesAHigherWorkOneBlockCompetingBranch)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    const auto first = MakeCoinbaseBlock(state->active_tip(), 1U, 1'100U, kRegularBits, parameters);
    ASSERT_TRUE(first.has_value());
    Accept(*state, *first, 1'200U, kRegularBits);
    const auto main = MakeCoinbaseBlock(BlockHash(*first), 2U, 1'200U, kRegularBits, parameters);
    ASSERT_TRUE(main.has_value());
    Accept(*state, *main, 1'300U, kRegularBits);
    const auto side = MakeCoinbaseBlock(BlockHash(*first), 2U, 1'210U, kRegularBits, parameters);
    ASSERT_TRUE(side.has_value());
    Accept(*state, *side, 1'310U, kRegularBits);

    const auto side_extension =
        MakeCoinbaseBlock(BlockHash(*side), 3U, 1'310U, kRegularBits, parameters);
    ASSERT_TRUE(side_extension.has_value());
    Accept(*state, *side_extension, 1'410U, kRegularBits);

    EXPECT_EQ(state->active_tip(), BlockHash(*side_extension));
    EXPECT_EQ(state->active_height(), 3U);
    EXPECT_EQ(utxos.size(), 3U);
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*side_extension, parameters)));
    EXPECT_FALSE(utxos.HaveCoin(CoinbaseOutputKey(*main, parameters)));
    const auto main_index = state->GetBlockIndex(BlockHash(*main));
    ASSERT_TRUE(main_index.has_value());
    EXPECT_EQ(main_index->status, BlockStatus::kValid);
    const auto fork = state->FindFork(BlockHash(*main), BlockHash(*side));
    ASSERT_TRUE(fork.has_value());
    EXPECT_EQ(*fork, BlockHash(*first));
}

TEST(ChainStateSelection, ReorganizesMultipleBlocksAndPreservesTheWinningUtxos)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    const auto first = MakeCoinbaseBlock(state->active_tip(), 1U, 1'100U, kRegularBits, parameters);
    ASSERT_TRUE(first.has_value());
    Accept(*state, *first, 1'200U, kRegularBits);
    const auto main_two =
        MakeCoinbaseBlock(BlockHash(*first), 2U, 1'200U, kRegularBits, parameters);
    ASSERT_TRUE(main_two.has_value());
    Accept(*state, *main_two, 1'300U, kRegularBits);
    const auto main_three =
        MakeCoinbaseBlock(BlockHash(*main_two), 3U, 1'300U, kRegularBits, parameters);
    ASSERT_TRUE(main_three.has_value());
    Accept(*state, *main_three, 1'400U, kRegularBits);

    const auto side_two =
        MakeCoinbaseBlock(BlockHash(*first), 2U, 1'210U, kRegularBits, parameters);
    ASSERT_TRUE(side_two.has_value());
    Accept(*state, *side_two, 1'310U, kRegularBits);
    const auto side_three =
        MakeCoinbaseBlock(BlockHash(*side_two), 3U, 1'310U, kRegularBits, parameters);
    ASSERT_TRUE(side_three.has_value());
    Accept(*state, *side_three, 1'410U, kRegularBits);
    const auto side_four =
        MakeCoinbaseBlock(BlockHash(*side_three), 4U, 1'410U, kRegularBits, parameters);
    ASSERT_TRUE(side_four.has_value());
    Accept(*state, *side_four, 1'510U, kRegularBits);

    EXPECT_EQ(state->active_tip(), BlockHash(*side_four));
    EXPECT_EQ(state->active_height(), 4U);
    EXPECT_EQ(utxos.size(), 4U);
    const auto old_tip = state->GetBlockIndex(BlockHash(*main_three));
    ASSERT_TRUE(old_tip.has_value());
    EXPECT_EQ(old_tip->status, BlockStatus::kValid);
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*side_two, parameters)));
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*side_three, parameters)));
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*side_four, parameters)));
    EXPECT_FALSE(utxos.HaveCoin(CoinbaseOutputKey(*main_two, parameters)));
    EXPECT_FALSE(utxos.HaveCoin(CoinbaseOutputKey(*main_three, parameters)));
}

TEST(ChainStateSelection, RejectsInvalidCompetingBlocksWithoutChangingActiveState)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    const auto first = MakeCoinbaseBlock(state->active_tip(), 1U, 1'100U, kRegularBits, parameters);
    ASSERT_TRUE(first.has_value());
    Accept(*state, *first, 1'200U, kRegularBits);
    const auto active_before = state->active_tip();
    const auto size_before = utxos.size();
    const auto known_before = state->known_block_count();

    auto invalid = MakeCoinbaseBlock(active_before, 2U, 1'200U, kRegularBits, parameters);
    ASSERT_TRUE(invalid.has_value());
    invalid->transactions.front().outputs.front().value += 1LL;
    const auto root = nova::primitives::ComputeMerkleRoot(
        invalid->transactions, parameters.block_validation.block_limits.transaction_limits);
    ASSERT_TRUE(root.has_value());
    invalid->header.merkle_root = *root;
    const auto target = nova::consensus::TargetFromCompact(kRegularBits);
    ASSERT_TRUE(target.target.has_value());
    bool mined = false;
    for (std::uint32_t nonce = 0U; nonce < 4'096U; ++nonce) {
        invalid->header.nonce = nonce;
        const auto hash = nova::primitives::ComputeBlockHash(invalid->header);
        ASSERT_TRUE(hash.has_value());
        if (nova::consensus::CheckProofOfWork(*hash, *target.target)) {
            mined = true;
            break;
        }
    }
    ASSERT_TRUE(mined);
    const auto result = state->AcceptBlock(*invalid);
    EXPECT_EQ(result.error, ChainError::kBlockValidationFailure);
    EXPECT_EQ(result.validation_error, nova::chain::BlockValidationError::kInvalidCoinbaseReward);
    EXPECT_EQ(state->active_tip(), active_before);
    EXPECT_EQ(utxos.size(), size_before);
    EXPECT_EQ(state->known_block_count(), known_before);
}

TEST(ChainStateSecurity, RejectsFarFutureHeadersWithoutChangingTheActiveTip)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    const auto original_tip = state->active_tip();
    const auto far_future = MakeCoinbaseBlock(original_tip, 1U, 10'601U, kRegularBits, parameters);
    ASSERT_TRUE(far_future.has_value());

    const auto rejected = state->AcceptBlock(*far_future);
    EXPECT_EQ(rejected.error, ChainError::kBlockValidationFailure);
    EXPECT_EQ(rejected.validation_error, nova::chain::BlockValidationError::kTimeTooNew);
    EXPECT_EQ(state->active_tip(), original_tip);

    const auto normal = MakeCoinbaseBlock(original_tip, 1U, 1'100U, kRegularBits, parameters);
    ASSERT_TRUE(normal.has_value());
    EXPECT_EQ(state->AcceptBlock(*normal).error, ChainError::kNone);
    EXPECT_EQ(state->active_tip(), BlockHash(*normal));
}

} // namespace
