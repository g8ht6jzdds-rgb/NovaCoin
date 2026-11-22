#include <gtest/gtest.h>

#include "chain/chainstate.hpp"
#include "chain/mempool.hpp"
#include "chain/network_chainstate.hpp"
#include <new>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace
{

using nova::chain::BlockStatus;
using nova::chain::BlockValidationError;
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

    const auto active = state->GetActiveBlockIndexes();
    ASSERT_TRUE(active.has_value());
    ASSERT_EQ(active->size(), 3U);
    EXPECT_EQ(active->at(0U).height, 0U);
    EXPECT_EQ(active->at(1U).hash, BlockHash(*first));
    EXPECT_EQ(active->at(2U).hash, BlockHash(*second));
}

TEST(ChainStateDifficulty, MissingTestnetRecoveryAncestryIsUnavailableContext)
{
    const auto& network = nova::consensus::TestnetNetworkParams();
    const auto target =
        nova::consensus::TargetFromCompact(network.pow.pow_limit_compact, network.pow);
    ASSERT_TRUE(target.target.has_value());
    const auto work = nova::consensus::CalculateWork(*target.target);
    ASSERT_TRUE(work.has_value());
    const auto anchor_hash = HashWithLastByte(0xB9U);
    const auto anchor_time =
        network.genesis_block.header.time + network.difficulty.target_spacing_seconds;
    auto parameters = ChainStateParams{
        nova::chain::NetworkValidationParams(network), network.difficulty, 11U,
        std::make_shared<nova::chain::FixedValidationTimeSource>(anchor_time + 100'000U)};
    UTXOSet controlled;
    auto state = ChainState::Create(parameters,
                                    {anchor_hash,
                                     {},
                                     1U,
                                     anchor_time,
                                     *target.target,
                                     *work,
                                     ChainWork::FromPerBlockWork(*work)},
                                    controlled);
    ASSERT_NE(state, nullptr);
    for (const auto delay : {network.difficulty.target_spacing_seconds,
                             network.difficulty.target_spacing_seconds * 2U}) {
        const auto block = MakeCoinbaseBlock(anchor_hash, 2U, anchor_time + delay,
                                             network.pow.pow_limit_compact, parameters);
        ASSERT_TRUE(block.has_value());
        const auto result = state->AcceptBlock(*block);
        EXPECT_EQ(result.error, ChainError::kContextFailure);
        EXPECT_EQ(result.validation_error, BlockValidationError::kInvalidContext);
        EXPECT_FALSE(state->GetBlockIndex(BlockHash(*block)).has_value());
        EXPECT_EQ(state->active_tip(), anchor_hash);
        EXPECT_EQ(state->active_height(), 1U);
        EXPECT_EQ(controlled.size(), 0U);
    }
    // The strictly delayed rule needs only the known parent, not a recovery walk.
    const auto delayed = MakeCoinbaseBlock(
        anchor_hash, 2U, anchor_time + network.difficulty.target_spacing_seconds * 2U + 1U,
        network.pow.pow_limit_compact, parameters);
    ASSERT_TRUE(delayed.has_value());
    EXPECT_EQ(state->AcceptBlock(*delayed).error, ChainError::kNone);
    EXPECT_EQ(state->active_tip(), BlockHash(*delayed));
    EXPECT_EQ(controlled.size(), 1U);
}

TEST(ChainStateDifficulty, SelfLinkedTestnetDifficultyAncestryIsUnavailableContext)
{
    const auto& network = nova::consensus::TestnetNetworkParams();
    const auto target =
        nova::consensus::TargetFromCompact(network.pow.pow_limit_compact, network.pow);
    ASSERT_TRUE(target.target.has_value());
    const auto work = nova::consensus::CalculateWork(*target.target);
    ASSERT_TRUE(work.has_value());
    const auto anchor_hash = HashWithLastByte(0xB9U);
    const auto anchor_time =
        network.genesis_block.header.time + network.difficulty.target_spacing_seconds;
    auto parameters = ChainStateParams{
        nova::chain::NetworkValidationParams(network), network.difficulty, 11U,
        std::make_shared<nova::chain::FixedValidationTimeSource>(anchor_time + 100'000U)};
    for (const auto height : {1U, network.difficulty.retarget_interval - 1U}) {
        UTXOSet controlled;
        auto state = ChainState::Create(parameters,
                                        {anchor_hash, anchor_hash, height, anchor_time,
                                         *target.target, *work, ChainWork::FromPerBlockWork(*work)},
                                        controlled);
        ASSERT_NE(state, nullptr);
        for (const auto delay : {network.difficulty.target_spacing_seconds,
                                 network.difficulty.target_spacing_seconds * 2U}) {
            const auto block = MakeCoinbaseBlock(anchor_hash, height + 1U, anchor_time + delay,
                                                 network.pow.pow_limit_compact, parameters);
            ASSERT_TRUE(block.has_value());
            const auto result = state->AcceptBlock(*block);
            EXPECT_EQ(result.error, ChainError::kContextFailure);
            EXPECT_EQ(result.validation_error, BlockValidationError::kInvalidContext);
            EXPECT_FALSE(state->GetBlockIndex(BlockHash(*block)).has_value());
            EXPECT_EQ(state->active_tip(), anchor_hash);
            EXPECT_EQ(state->active_height(), height);
            EXPECT_EQ(controlled.size(), 0U);
        }
    }
}

TEST(ChainStateDifficulty, NoRetargetingInheritsParentExpectedBits)
{
    UTXOSet utxos;
    auto parameters = TestParameters();
    constexpr std::uint32_t kToughAnchorBits = 0x1F00'FFFFU;
    const auto target = nova::consensus::TargetFromCompact(
        kToughAnchorBits, parameters.block_validation.pow_parameters);
    ASSERT_TRUE(target.target.has_value());
    const auto work = nova::consensus::CalculateWork(*target.target);
    ASSERT_TRUE(work.has_value());
    auto state =
        ChainState::Create(parameters,
                           ChainAnchor{HashWithLastByte(0xB0U), nova::crypto::Hash256{}, 0U, 1'000U,
                                       *target.target, *work, ChainWork::FromPerBlockWork(*work)},
                           utxos);
    ASSERT_NE(state, nullptr);
    const auto original_tip = state->active_tip();
    const auto original_height = state->active_height();
    const auto original_utxo_count = utxos.size();

    Block inherited{{1, state->active_tip(), nova::crypto::Hash256{}, 1'100U, kToughAnchorBits, 0U},
                    {}};
    const auto inherited_result = state->AcceptBlock(inherited);
    EXPECT_EQ(inherited_result.error, ChainError::kBlockValidationFailure);
    EXPECT_EQ(inherited_result.validation_error, BlockValidationError::kBadProofOfWork);

    Block pow_limit{{1, state->active_tip(), nova::crypto::Hash256{}, 1'100U,
                     parameters.block_validation.pow_parameters.pow_limit_compact, 1U},
                    {}};
    const auto pow_limit_result = state->AcceptBlock(pow_limit);
    EXPECT_EQ(pow_limit_result.error, ChainError::kBlockValidationFailure);
    EXPECT_EQ(pow_limit_result.validation_error, BlockValidationError::kUnexpectedBits);
    EXPECT_EQ(state->active_tip(), original_tip);
    EXPECT_EQ(state->active_height(), original_height);
    EXPECT_EQ(utxos.size(), original_utxo_count);
}

TEST(ChainStateDifficulty, RetargetContextRequiresPositiveQuarterTimespan)
{
    const auto target = nova::consensus::TargetFromCompact(kRegularBits);
    ASSERT_TRUE(target.target.has_value());
    const auto work = nova::consensus::CalculateWork(*target.target);
    ASSERT_TRUE(work.has_value());
    for (const auto no_retargeting : {false, true}) {
        for (std::uint32_t timespan = 0U; timespan <= 4U; ++timespan) {
            SCOPED_TRACE(timespan);
            SCOPED_TRACE(no_retargeting);
            UTXOSet controlled;
            auto parameters = TestParameters();
            parameters.difficulty = {timespan, 1U, timespan, false, no_retargeting};
            const auto state = ChainState::Create(parameters,
                                                  {HashWithLastByte(0xA0U),
                                                   {},
                                                   0U,
                                                   1'000U,
                                                   *target.target,
                                                   *work,
                                                   ChainWork::FromPerBlockWork(*work)},
                                                  controlled);
            EXPECT_EQ(state != nullptr, timespan != 0U && (no_retargeting || timespan >= 4U));
            EXPECT_EQ(controlled.size(), 0U);
        }
    }
}

TEST(ChainWork, AccumulatesExactRegtestPowLimitWork)
{
    const auto work =
        nova::consensus::CalculateWork(nova::consensus::RegtestPowParameters().pow_limit);
    ASSERT_TRUE(work.has_value());

    auto accumulated = ChainWork::FromPerBlockWork(*work);
    ASSERT_TRUE(accumulated.Add(*work));
    ChainWork::Bytes expected{};
    expected.back() = 4U;
    EXPECT_EQ(accumulated.bytes(), expected);
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

TEST(ChainStateAtomicity, RejectedRollbackWithDescendantPreservesPublishedState)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    const auto anchor = state->active_tip();
    const auto first = MakeCoinbaseBlock(anchor, 1U, 1'100U, kRegularBits, parameters);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(state->AcceptBlock(*first).error, ChainError::kNone);
    const auto second = MakeCoinbaseBlock(BlockHash(*first), 2U, 1'200U, kRegularBits, parameters);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(state->AcceptBlock(*second).error, ChainError::kNone);
    const auto tip_before = state->active_tip();
    const auto known_before = state->known_block_count();
    const auto result = state->RollbackAcceptedBlock(BlockHash(*first), anchor);
    EXPECT_EQ(result.error, ChainError::kReorganizationFailure);
    EXPECT_EQ(state->active_tip(), tip_before);
    EXPECT_EQ(state->active_height(), 2U);
    EXPECT_EQ(state->known_block_count(), known_before);
    EXPECT_EQ(utxos.size(), 2U);
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*first, parameters)));
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*second, parameters)));
}

TEST(ChainStateAtomicity, StagingFailuresPreserveStateAndAreConsumedOnce)
{
    using Failure = ChainState::FailurePoint;
    for (const auto point :
         {Failure::kBeforeStaging, Failure::kAfterCopy, Failure::kAfterTransition}) {
        SCOPED_TRACE(static_cast<unsigned>(point));
        UTXOSet utxos;
        auto state = CreateState(utxos);
        ASSERT_NE(state, nullptr);
        const auto* stable_state = state.get();
        const auto* stable_utxos = &utxos;
        const auto parameters = TestParameters();
        const auto anchor = state->active_tip();
        const auto first = MakeCoinbaseBlock(anchor, 1U, 1'100U, kRegularBits, parameters);
        ASSERT_TRUE(first.has_value());
        ASSERT_EQ(state->AcceptBlock(*first).error, ChainError::kNone);
        const auto second =
            MakeCoinbaseBlock(BlockHash(*first), 2U, 1'200U, kRegularBits, parameters);
        ASSERT_TRUE(second.has_value());
        const auto first_key = CoinbaseOutputKey(*first, parameters);
        const auto second_key = CoinbaseOutputKey(*second, parameters);
        const auto first_coin = utxos.GetCoin(first_key);
        ASSERT_TRUE(first_coin.has_value());
        const auto first_index = state->GetBlockIndex(BlockHash(*first));
        ASSERT_TRUE(first_index.has_value());
        const auto known_before = state->known_block_count();
        state->SetFailurePointForTesting(point);
        EXPECT_EQ(state->AcceptBlock(*second).error, ChainError::kAllocationFailure);
        EXPECT_EQ(state->active_tip(), BlockHash(*first));
        EXPECT_EQ(state->active_height(), 1U);
        EXPECT_EQ(state->known_block_count(), known_before);
        EXPECT_FALSE(state->GetBlockIndex(BlockHash(*second)).has_value());
        EXPECT_FALSE(utxos.HaveCoin(second_key));
        ASSERT_EQ(utxos.size(), 1U);
        const auto unchanged_coin = utxos.GetCoin(first_key);
        ASSERT_TRUE(unchanged_coin.has_value());
        EXPECT_EQ(unchanged_coin->output.value, first_coin->output.value);
        EXPECT_EQ(unchanged_coin->output.script_pubkey, first_coin->output.script_pubkey);
        EXPECT_EQ(unchanged_coin->height, first_coin->height);
        EXPECT_EQ(unchanged_coin->is_coinbase, first_coin->is_coinbase);
        const auto first_preserved = state->GetBlockIndex(BlockHash(*first));
        ASSERT_TRUE(first_preserved.has_value());
        EXPECT_EQ(first_preserved->chain_work, first_index->chain_work);
        ASSERT_EQ(state->AcceptBlock(*second).error, ChainError::kNone);
        EXPECT_EQ(state.get(), stable_state);
        EXPECT_EQ(&utxos, stable_utxos);
        const auto second_index = state->GetBlockIndex(BlockHash(*second));
        ASSERT_TRUE(second_index.has_value());
        for (const bool rollback : {false, true}) {
            state->SetFailurePointForTesting(point);
            const auto result =
                rollback ? state->RollbackAcceptedBlock(BlockHash(*second), BlockHash(*first))
                         : state->ActivateBestChain();
            EXPECT_EQ(result.error, ChainError::kAllocationFailure);
            EXPECT_EQ(state->active_tip(), BlockHash(*second));
            EXPECT_EQ(state->active_height(), 2U);
            EXPECT_EQ(state->known_block_count(), known_before + 1U);
            ASSERT_EQ(utxos.size(), 2U);
            EXPECT_TRUE(utxos.HaveCoin(first_key));
            EXPECT_TRUE(utxos.HaveCoin(second_key));
            const auto second_preserved = state->GetBlockIndex(BlockHash(*second));
            ASSERT_TRUE(second_preserved.has_value());
            EXPECT_EQ(second_preserved->chain_work, second_index->chain_work);
        }
        ASSERT_EQ(state->RollbackAcceptedBlock(BlockHash(*second), BlockHash(*first)).error,
                  ChainError::kNone);
        EXPECT_EQ(state->active_tip(), BlockHash(*first));
        EXPECT_FALSE(state->GetBlockIndex(BlockHash(*second)).has_value());
        EXPECT_EQ(utxos.size(), 1U);
        EXPECT_TRUE(utxos.HaveCoin(first_key));
        EXPECT_FALSE(utxos.HaveCoin(second_key));
        EXPECT_EQ(state->ActivateBestChain().error, ChainError::kNone);
    }
}

TEST(ChainStateAtomicity, SparseReorgPreservesUnchangedAndDisconnectedIndexStatuses)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    std::vector<nova::crypto::Hash256> known{state->active_tip()};
    const auto verify = [&]() {
        const auto active = state->GetActiveBlockIndexes();
        ASSERT_TRUE(active.has_value());
        for (const auto& hash : known) {
            const auto index = state->GetBlockIndex(hash);
            ASSERT_TRUE(index.has_value());
            const auto on_active =
                std::find_if(active->begin(), active->end(),
                             [&](const auto& item) { return item.hash == hash; }) != active->end();
            EXPECT_EQ(index->status, on_active ? BlockStatus::kActive : BlockStatus::kValid);
            if (index->block.has_value()) {
                const auto coin = utxos.GetCoin(CoinbaseOutputKey(*index->block, parameters));
                EXPECT_EQ(coin.has_value(), on_active);
                if (coin.has_value()) {
                    EXPECT_EQ(coin->output.value,
                              index->block->transactions.front().outputs.front().value);
                    EXPECT_EQ(coin->output.script_pubkey,
                              index->block->transactions.front().outputs.front().script_pubkey);
                    EXPECT_EQ(coin->height, index->height);
                    EXPECT_TRUE(coin->is_coinbase);
                }
            }
        }
    };
    const auto shared = MakeCoinbaseBlock(known.front(), 1U, 1100U, kRegularBits, parameters);
    ASSERT_TRUE(shared.has_value());
    ASSERT_EQ(state->AcceptBlock(*shared).error, ChainError::kNone);
    known.push_back(BlockHash(*shared));
    auto parent = known.back();
    for (const auto height : {2U, 3U}) {
        const auto block =
            MakeCoinbaseBlock(parent, height, 1000U + height * 100U, kRegularBits, parameters);
        ASSERT_TRUE(block.has_value());
        ASSERT_EQ(state->AcceptBlock(*block).error, ChainError::kNone);
        parent = BlockHash(*block);
        known.push_back(parent);
        verify();
    }
    parent = BlockHash(*shared);
    for (const auto height : {2U, 3U, 4U}) {
        const auto side =
            MakeCoinbaseBlock(parent, height, 1010U + height * 100U, kRegularBits, parameters);
        ASSERT_TRUE(side.has_value());
        if (height == 4U) {
            const auto before_tip = state->active_tip();
            const auto before_count = state->known_block_count();
            std::vector<BlockStatus> statuses;
            for (const auto& hash : known) {
                const auto index = state->GetBlockIndex(hash);
                ASSERT_TRUE(index.has_value());
                statuses.push_back(index->status);
            }
            state->SetFailurePointForTesting(ChainState::FailurePoint::kAfterTransition);
            EXPECT_EQ(state->AcceptBlock(*side).error, ChainError::kAllocationFailure);
            EXPECT_EQ(state->active_tip(), before_tip);
            EXPECT_EQ(state->known_block_count(), before_count);
            EXPECT_FALSE(state->GetBlockIndex(BlockHash(*side)).has_value());
            for (std::size_t i = 0U; i < known.size(); ++i) {
                const auto index = state->GetBlockIndex(known[i]);
                ASSERT_TRUE(index.has_value());
                EXPECT_EQ(index->status, statuses[i]);
            }
            verify();
        }
        ASSERT_EQ(state->AcceptBlock(*side).error, ChainError::kNone);
        parent = BlockHash(*side);
        known.push_back(parent);
        verify();
    }
    EXPECT_EQ(state->active_tip(), parent);
    EXPECT_EQ(state->active_height(), 4U);
    EXPECT_EQ(utxos.size(), 4U);
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*shared, parameters)));
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

TEST(ChainStatePrepared, KeepsTransitionPrivateUntilExplicitOneShotCommit)
{
    static_assert(!std::is_move_constructible_v<ChainState>);
    static_assert(!std::is_copy_constructible_v<ChainState::PreparedTransition>);
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto anchor = state->active_tip();
    const auto block = MakeCoinbaseBlock(anchor, 1U, 1100U, kRegularBits, TestParameters());
    ASSERT_TRUE(block.has_value());
    auto prepared = state->PrepareBlock(*block);
    ASSERT_EQ(prepared.result.error, ChainError::kNone);
    ASSERT_NE(prepared.prepared, nullptr);
    EXPECT_TRUE(prepared.result.became_active);
    EXPECT_TRUE(state->CanCommitPrepared(*prepared.prepared));
    EXPECT_EQ(state->active_tip(), anchor);
    EXPECT_EQ(state->active_height(), 0U);
    EXPECT_EQ(state->known_block_count(), 1U);
    EXPECT_EQ(utxos.size(), 0U);
    EXPECT_FALSE(state->GetBlockIndex(BlockHash(*block)).has_value());
    const auto view = state->GetPreparedActiveBlockIndexes(*prepared.prepared);
    ASSERT_TRUE(view.has_value());
    ASSERT_EQ(view->size(), 2U);
    EXPECT_EQ(view->back().hash, BlockHash(*block));
    EXPECT_EQ(view->back().height, 1U);
    EXPECT_EQ(state->CommitPrepared(*prepared.prepared).error, ChainError::kNone);
    EXPECT_EQ(state->active_tip(), BlockHash(*block));
    EXPECT_EQ(state->known_block_count(), 2U);
    EXPECT_EQ(utxos.size(), 1U);
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*block, TestParameters())));
    EXPECT_FALSE(state->CanCommitPrepared(*prepared.prepared));
    EXPECT_FALSE(state->GetPreparedActiveBlockIndexes(*prepared.prepared).has_value());
    EXPECT_EQ(state->CommitPrepared(*prepared.prepared).error, ChainError::kContextFailure);
}

TEST(ChainStatePrepared, WrongOwnerDestroyedOwnerAndSiblingCannotPublish)
{
    UTXOSet utxos;
    UTXOSet other_utxos;
    auto state = CreateState(utxos);
    auto other = CreateState(other_utxos);
    ASSERT_NE(state, nullptr);
    ASSERT_NE(other, nullptr);
    const auto block =
        MakeCoinbaseBlock(state->active_tip(), 1U, 1100U, kRegularBits, TestParameters());
    ASSERT_TRUE(block.has_value());
    auto first = state->PrepareBlock(*block);
    auto sibling = state->PrepareBlock(*block);
    ASSERT_NE(first.prepared, nullptr);
    ASSERT_NE(sibling.prepared, nullptr);
    EXPECT_FALSE(other->CanCommitPrepared(*first.prepared));
    EXPECT_FALSE(other->GetPreparedActiveBlockIndexes(*first.prepared).has_value());
    EXPECT_EQ(other->CommitPrepared(*first.prepared).error, ChainError::kContextFailure);
    EXPECT_EQ(other_utxos.size(), 0U);
    ASSERT_EQ(state->CommitPrepared(*first.prepared).error, ChainError::kNone);
    EXPECT_EQ(state->CommitPrepared(*sibling.prepared).error, ChainError::kContextFailure);
    EXPECT_FALSE(state->GetPreparedActiveBlockIndexes(*sibling.prepared).has_value());
    auto owned = other->PrepareBlock(*block);
    ASSERT_NE(owned.prepared, nullptr);
    other.reset();
    auto replacement = CreateState(other_utxos);
    ASSERT_NE(replacement, nullptr);
    EXPECT_FALSE(replacement->CanCommitPrepared(*owned.prepared));
    EXPECT_FALSE(replacement->GetPreparedActiveBlockIndexes(*owned.prepared).has_value());
    EXPECT_EQ(replacement->CommitPrepared(*owned.prepared).error, ChainError::kContextFailure);
    EXPECT_EQ(other_utxos.size(), 0U);
}

TEST(ChainStatePrepared, DirectUtxoChangesInvalidateBeforeBorrowedReads)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto block =
        MakeCoinbaseBlock(state->active_tip(), 1U, 1100U, kRegularBits, TestParameters());
    ASSERT_TRUE(block.has_value());
    auto prepared = state->PrepareBlock(*block);
    ASSERT_NE(prepared.prepared, nullptr);
    const UTXOKey extra{HashWithLastByte(0xEEU), 0U};
    ASSERT_TRUE(utxos.AddCoin(extra, nova::chain::Coin{TxOutput{1, {0x51U}}, 0U, false}));
    EXPECT_FALSE(state->CanCommitPrepared(*prepared.prepared));
    EXPECT_FALSE(state->GetPreparedActiveBlockIndexes(*prepared.prepared).has_value());
    EXPECT_EQ(state->CommitPrepared(*prepared.prepared).error, ChainError::kContextFailure);
    EXPECT_EQ(state->active_height(), 0U);
    EXPECT_EQ(utxos.size(), 1U);
    auto before_spend = state->PrepareBlock(*block);
    ASSERT_NE(before_spend.prepared, nullptr);
    ASSERT_TRUE(utxos.SpendCoin(extra).has_value());
    EXPECT_FALSE(state->CanCommitPrepared(*before_spend.prepared));
    EXPECT_EQ(state->CommitPrepared(*before_spend.prepared).error, ChainError::kContextFailure);
    EXPECT_EQ(utxos.size(), 0U);
}

TEST(ChainStatePrepared, AbortInvalidBlockAndPreparationFailuresNeverPublish)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto anchor = state->active_tip();
    const auto block = MakeCoinbaseBlock(anchor, 1U, 1100U, kRegularBits, TestParameters());
    ASSERT_TRUE(block.has_value());
    auto abandoned = state->PrepareBlock(*block);
    ASSERT_NE(abandoned.prepared, nullptr);
    abandoned.prepared.reset();
    auto invalid = *block;
    invalid.header.merkle_root = nova::crypto::Hash256{};
    const auto rejected = state->PrepareBlock(invalid);
    EXPECT_NE(rejected.result.error, ChainError::kNone);
    EXPECT_EQ(rejected.prepared, nullptr);
    for (const auto point :
         {ChainState::FailurePoint::kBeforeStaging, ChainState::FailurePoint::kAfterCopy,
          ChainState::FailurePoint::kAfterTransition}) {
        state->SetFailurePointForTesting(point);
        const auto failed = state->PrepareBlock(*block);
        EXPECT_EQ(failed.result.error, ChainError::kAllocationFailure);
        EXPECT_EQ(failed.prepared, nullptr);
        EXPECT_EQ(state->active_tip(), anchor);
        EXPECT_EQ(state->known_block_count(), 1U);
        EXPECT_EQ(utxos.size(), 0U);
    }
    const auto retry = state->PrepareBlock(*block);
    ASSERT_NE(retry.prepared, nullptr);
    EXPECT_EQ(state->active_tip(), anchor);
}

TEST(ChainStatePrepared, ActivationAndRollbackInvalidateOutstandingTransitions)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto anchor = state->active_tip();
    const auto first = MakeCoinbaseBlock(anchor, 1U, 1100U, kRegularBits, TestParameters());
    ASSERT_TRUE(first.has_value());
    auto pending = state->PrepareBlock(*first);
    ASSERT_NE(pending.prepared, nullptr);
    ASSERT_EQ(state->ActivateBestChain().error, ChainError::kNone);
    EXPECT_FALSE(state->CanCommitPrepared(*pending.prepared));
    ASSERT_EQ(state->AcceptBlock(*first).error, ChainError::kNone);
    const auto second =
        MakeCoinbaseBlock(state->active_tip(), 2U, 1200U, kRegularBits, TestParameters());
    ASSERT_TRUE(second.has_value());
    auto next = state->PrepareBlock(*second);
    ASSERT_NE(next.prepared, nullptr);
    ASSERT_EQ(state->RollbackAcceptedBlock(BlockHash(*first), anchor).error, ChainError::kNone);
    EXPECT_FALSE(state->CanCommitPrepared(*next.prepared));
    EXPECT_FALSE(state->GetPreparedActiveBlockIndexes(*next.prepared).has_value());
    EXPECT_EQ(state->CommitPrepared(*next.prepared).error, ChainError::kContextFailure);
    EXPECT_EQ(state->active_tip(), anchor);
    EXPECT_EQ(utxos.size(), 0U);
}

#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool count_chain_publication_allocations = false;
thread_local std::size_t chain_publication_allocations = 0U;
int CountChainPublicationAllocations(const int kind, void*, std::size_t, int, long,
                                     const unsigned char*, int)
{
    if (count_chain_publication_allocations && kind == _HOOK_ALLOC)
        ++chain_publication_allocations;
    return 1;
}
#endif

TEST(ChainStatePrepared, ExplicitCommitDoesNotAllocate)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto block =
        MakeCoinbaseBlock(state->active_tip(), 1U, 1100U, kRegularBits, TestParameters());
    ASSERT_TRUE(block.has_value());
    auto prepared = state->PrepareBlock(*block);
    ASSERT_NE(prepared.prepared, nullptr);
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto previous = _CrtSetAllocHook(CountChainPublicationAllocations);
    chain_publication_allocations = 0U;
    count_chain_publication_allocations = true;
#endif
    const auto result = state->CommitPrepared(*prepared.prepared);
#if defined(_MSC_VER) && defined(_DEBUG)
    count_chain_publication_allocations = false;
    _CrtSetAllocHook(previous);
    EXPECT_EQ(chain_publication_allocations, 0U);
#endif
    EXPECT_EQ(result.error, ChainError::kNone);
    EXPECT_EQ(state->active_tip(), BlockHash(*block));
    EXPECT_EQ(utxos.size(), 1U);
}

TEST(ChainStatePrepared, PreparedReorganizationRetainsOldCoinsUntilCommit)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    const auto first = MakeCoinbaseBlock(state->active_tip(), 1U, 1100U, kRegularBits, parameters);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(state->AcceptBlock(*first).error, ChainError::kNone);
    const auto main_two = MakeCoinbaseBlock(BlockHash(*first), 2U, 1200U, kRegularBits, parameters);
    ASSERT_TRUE(main_two.has_value());
    ASSERT_EQ(state->AcceptBlock(*main_two).error, ChainError::kNone);
    const auto main_three =
        MakeCoinbaseBlock(BlockHash(*main_two), 3U, 1300U, kRegularBits, parameters);
    ASSERT_TRUE(main_three.has_value());
    ASSERT_EQ(state->AcceptBlock(*main_three).error, ChainError::kNone);
    const auto side_two = MakeCoinbaseBlock(BlockHash(*first), 2U, 1210U, kRegularBits, parameters);
    ASSERT_TRUE(side_two.has_value());
    ASSERT_EQ(state->AcceptBlock(*side_two).error, ChainError::kNone);
    ASSERT_EQ(state->active_tip(), BlockHash(*main_three));
    // Select a deterministic equal-work competitor winning the documented
    // hash tie-break, so this exercises a real two-block disconnect/connect.
    std::optional<Block> side_three;
    for (std::uint32_t time = 1310U; time < 1600U; ++time) {
        auto candidate =
            MakeCoinbaseBlock(BlockHash(*side_two), 3U, time, kRegularBits, parameters);
        ASSERT_TRUE(candidate.has_value());
        if (BlockHash(*candidate).bytes() < BlockHash(*main_three).bytes()) {
            side_three = std::move(candidate);
            break;
        }
    }
    ASSERT_TRUE(side_three.has_value());
    auto prepared = state->PrepareBlock(*side_three);
    ASSERT_NE(prepared.prepared, nullptr);
    ASSERT_TRUE(prepared.result.became_active);
    const auto view = state->GetPreparedActiveBlockIndexes(*prepared.prepared);
    ASSERT_TRUE(view.has_value());
    ASSERT_EQ(view->size(), 4U);
    EXPECT_EQ(view->back().hash, BlockHash(*side_three));
    EXPECT_EQ(state->active_tip(), BlockHash(*main_three));
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*main_two, parameters)));
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*main_three, parameters)));
    EXPECT_FALSE(utxos.HaveCoin(CoinbaseOutputKey(*side_two, parameters)));
    EXPECT_FALSE(utxos.HaveCoin(CoinbaseOutputKey(*side_three, parameters)));
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto previous = _CrtSetAllocHook(CountChainPublicationAllocations);
    chain_publication_allocations = 0U;
    count_chain_publication_allocations = true;
#endif
    const auto result = state->CommitPrepared(*prepared.prepared);
#if defined(_MSC_VER) && defined(_DEBUG)
    count_chain_publication_allocations = false;
    _CrtSetAllocHook(previous);
    EXPECT_EQ(chain_publication_allocations, 0U);
#endif
    ASSERT_EQ(result.error, ChainError::kNone);
    EXPECT_EQ(state->active_tip(), BlockHash(*side_three));
    EXPECT_EQ(utxos.size(), 3U);
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*first, parameters)));
    EXPECT_FALSE(utxos.HaveCoin(CoinbaseOutputKey(*main_two, parameters)));
    EXPECT_FALSE(utxos.HaveCoin(CoinbaseOutputKey(*main_three, parameters)));
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*side_two, parameters)));
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*side_three, parameters)));
    const auto old_index = state->GetBlockIndex(BlockHash(*main_three));
    const auto new_index = state->GetBlockIndex(BlockHash(*side_three));
    ASSERT_TRUE(old_index.has_value());
    ASSERT_TRUE(new_index.has_value());
    EXPECT_EQ(old_index->chain_work, new_index->chain_work);
    EXPECT_EQ(old_index->status, BlockStatus::kValid);
    EXPECT_EQ(new_index->status, BlockStatus::kActive);
}

TEST(ChainStatePrepared, InactiveBlockPublicationInvalidatesWithoutChangingActiveCoins)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    const auto anchor = state->active_tip();
    const auto first = MakeCoinbaseBlock(anchor, 1U, 1100U, kRegularBits, parameters);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(state->AcceptBlock(*first).error, ChainError::kNone);
    const auto second = MakeCoinbaseBlock(BlockHash(*first), 2U, 1200U, kRegularBits, parameters);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(state->AcceptBlock(*second).error, ChainError::kNone);
    const auto next = MakeCoinbaseBlock(BlockHash(*second), 3U, 1300U, kRegularBits, parameters);
    const auto side = MakeCoinbaseBlock(anchor, 1U, 1110U, kRegularBits, parameters);
    ASSERT_TRUE(next.has_value());
    ASSERT_TRUE(side.has_value());
    auto pending = state->PrepareBlock(*next);
    auto inactive = state->PrepareBlock(*side);
    ASSERT_NE(pending.prepared, nullptr);
    ASSERT_NE(inactive.prepared, nullptr);
    EXPECT_FALSE(inactive.result.became_active);
    ASSERT_EQ(state->CommitPrepared(*inactive.prepared).error, ChainError::kNone);
    EXPECT_EQ(state->active_tip(), BlockHash(*second));
    EXPECT_EQ(state->known_block_count(), 4U);
    EXPECT_EQ(utxos.size(), 2U);
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*first, parameters)));
    EXPECT_TRUE(utxos.HaveCoin(CoinbaseOutputKey(*second, parameters)));
    EXPECT_FALSE(utxos.HaveCoin(CoinbaseOutputKey(*side, parameters)));
    EXPECT_FALSE(state->CanCommitPrepared(*pending.prepared));
    EXPECT_FALSE(state->GetPreparedActiveBlockIndexes(*pending.prepared).has_value());
    EXPECT_EQ(state->CommitPrepared(*pending.prepared).error, ChainError::kContextFailure);
}

TEST(ChainStatePreparedReadOnly, VisitorCannotPublishAnOverlayIntoValidatedState)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    const auto block = MakeCoinbaseBlock(state->active_tip(), 1U, 1100U, kRegularBits, parameters);
    ASSERT_TRUE(block.has_value());
    auto prepared = state->PrepareBlock(*block);
    ASSERT_NE(prepared.prepared, nullptr);
    const auto key = CoinbaseOutputKey(*block, parameters);
    bool visited = false;
    EXPECT_TRUE(state->WithPreparedUTXO(*prepared.prepared, [&](const nova::chain::UTXOView& view) {
        visited = true;
        EXPECT_TRUE(view.HaveCoin(key));
        auto overlay = nova::chain::UTXOBatch::ReadOnlyOverlay(view);
        EXPECT_TRUE(overlay.has_value());
        if (!overlay.has_value())
            return false;
        Transaction spend{
            1,
            {TxInput{OutPoint{key.transaction_id, key.output_index}, {0x51U}, 0xFFFFFFFFU}},
            {TxOutput{1, {0x51U}}},
            0U};
        const auto applied = overlay->ApplyTransaction(
            spend, 2U, false, parameters.block_validation.block_limits.transaction_limits);
        EXPECT_EQ(applied.error, nova::chain::UTXOError::kNone);
        EXPECT_FALSE(overlay->Commit());
        EXPECT_TRUE(view.HaveCoin(key));
        return true;
    }));
    EXPECT_TRUE(visited);
    EXPECT_EQ(state->active_height(), 0U);
    EXPECT_EQ(utxos.size(), 0U);
    ASSERT_EQ(state->CommitPrepared(*prepared.prepared).error, ChainError::kNone);
    EXPECT_TRUE(utxos.HaveCoin(key));
    EXPECT_EQ(utxos.size(), 1U);
    EXPECT_FALSE(state->WithPreparedUTXO(*prepared.prepared, [&](const nova::chain::UTXOView&) {
        ADD_FAILURE() << "Consumed token must not invoke visitor";
        return true;
    }));
}

TEST(ChainStatePreparedReadOnly, VisitorFailureExceptionAndGenerationChangeRejectOutputs)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto block =
        MakeCoinbaseBlock(state->active_tip(), 1U, 1100U, kRegularBits, TestParameters());
    ASSERT_TRUE(block.has_value());
    auto prepared = state->PrepareBlock(*block);
    ASSERT_NE(prepared.prepared, nullptr);
    EXPECT_FALSE(state->WithPreparedUTXO(*prepared.prepared,
                                         [](const nova::chain::UTXOView&) { return false; }));
    EXPECT_FALSE(state->WithPreparedUTXO(
        *prepared.prepared, [](const nova::chain::UTXOView&) -> bool { throw std::bad_alloc{}; }));
    EXPECT_TRUE(state->CanCommitPrepared(*prepared.prepared));
    // Deliberately violate non-reentrancy to test the defensive after-visit
    // guard. This does not promise rollback of a visitor's own live mutations.
    EXPECT_FALSE(state->WithPreparedUTXO(*prepared.prepared, [&](const nova::chain::UTXOView&) {
        return state->ActivateBestChain().error == ChainError::kNone;
    }));
    EXPECT_FALSE(state->CanCommitPrepared(*prepared.prepared));
    EXPECT_EQ(utxos.size(), 0U);
}

TEST(ChainStatePreparedReadOnly, MempoolPreparationConsumesOnlyLexicalConstView)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    auto pool = nova::chain::Mempool::Create({parameters.block_validation, 16U, 100000U, 0});
    ASSERT_NE(pool, nullptr);
    const auto block = MakeCoinbaseBlock(state->active_tip(), 1U, 1100U, kRegularBits, parameters);
    ASSERT_TRUE(block.has_value());
    auto chain_prepared = state->PrepareBlock(*block);
    ASSERT_NE(chain_prepared.prepared, nullptr);
    std::unique_ptr<nova::chain::Mempool::PreparedState> pool_prepared;
    const auto visited =
        state->WithPreparedUTXO(*chain_prepared.prepared, [&](const nova::chain::UTXOView& view) {
            EXPECT_TRUE(view.HaveCoin(CoinbaseOutputKey(*block, parameters)));
            auto result = pool->PrepareReconciliation({}, {}, {2U, 1000U}, view, 1U);
            if (result.result.error != nova::chain::MempoolError::kNone)
                return false;
            pool_prepared = std::move(result.prepared);
            return pool_prepared != nullptr;
        });
    ASSERT_TRUE(visited);
    ASSERT_NE(pool_prepared, nullptr);
    EXPECT_EQ(state->active_height(), 0U);
    EXPECT_EQ(utxos.size(), 0U);
    EXPECT_EQ(pool->size(), 0U);
    ASSERT_EQ(state->CommitPrepared(*chain_prepared.prepared).error, ChainError::kNone);
    ASSERT_TRUE(pool->CommitPrepared(*pool_prepared));
    EXPECT_EQ(state->active_height(), 1U);
    EXPECT_EQ(pool->size(), 0U);
    // No durable store or concurrent-reader atomicity is claimed by this test.
}

TEST(ChainStateReadFailure, LookupUnavailableIsContextFailureAndPublishesNothing)
{
    UTXOSet utxos;
    auto state = CreateState(utxos);
    ASSERT_NE(state, nullptr);
    const auto parameters = TestParameters();
    const auto anchor = state->active_tip();
    const UTXOKey funding{HashWithLastByte(0xEFU), 0U};
    ASSERT_TRUE(
        utxos.AddCoin(funding, nova::chain::Coin{TxOutput{50LL * COIN, {0x51U}}, 0U, false}));
    const auto control = MakeCoinbaseBlock(anchor, 1U, 1100U, kRegularBits, parameters);
    ASSERT_TRUE(control.has_value());
    auto block = *control;
    block.transactions.push_back(
        Transaction{1,
                    {TxInput{OutPoint{funding.transaction_id, 0U}, {0x51U}, 0xFFFFFFFFU}},
                    {TxOutput{49LL * COIN, {0x51U}}},
                    0U});
    const auto root = nova::primitives::ComputeMerkleRoot(
        block.transactions, parameters.block_validation.block_limits.transaction_limits);
    ASSERT_TRUE(root.has_value());
    block.header.merkle_root = *root;
    const auto target = nova::consensus::TargetFromCompact(kRegularBits);
    ASSERT_TRUE(target.target.has_value());
    bool mined = false;
    for (std::uint32_t nonce = 0U; nonce < 4096U; ++nonce) {
        block.header.nonce = nonce;
        if (nova::consensus::CheckProofOfWork(BlockHash(block), *target.target)) {
            mined = true;
            break;
        }
    }
    ASSERT_TRUE(mined);
    // Healthy context rejects the deliberately unsupported locking condition.
    const auto healthy_rejection = state->AcceptBlock(block);
    EXPECT_EQ(healthy_rejection.validation_error, BlockValidationError::kUnsupportedLockingScript);
    utxos.FailNextReadForTesting();
    const auto unavailable = state->AcceptBlock(block);
    EXPECT_EQ(unavailable.error, ChainError::kContextFailure);
    EXPECT_EQ(unavailable.validation_error, BlockValidationError::kInvalidContext);
    EXPECT_EQ(state->active_tip(), anchor);
    EXPECT_EQ(state->known_block_count(), 1U);
    EXPECT_EQ(utxos.size(), 1U);
    EXPECT_TRUE(utxos.HaveCoin(funding));
    const auto retry = state->AcceptBlock(block);
    EXPECT_EQ(retry.validation_error, BlockValidationError::kUnsupportedLockingScript);
    EXPECT_EQ(state->AcceptBlock(*control).error, ChainError::kNone);
    EXPECT_EQ(state->active_height(), 1U);
    EXPECT_EQ(utxos.size(), 2U);
}

} // namespace
