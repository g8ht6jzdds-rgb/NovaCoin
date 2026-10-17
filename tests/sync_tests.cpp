#include <gtest/gtest.h>

#include "node/sync.hpp"

#include <limits>
#include <stdexcept>

namespace
{
using nova::crypto::Hash256;
using nova::node::SyncError;
using nova::primitives::Block;

Hash256 Hash(const Block& block)
{
    return nova::primitives::ComputeBlockHash(block.header).value();
}

class SyncProgress : public testing::Test
{
  protected:
    const nova::consensus::NetworkParams& network = nova::consensus::RegtestNetworkParams();
    nova::chain::UTXOSet utxos;
    std::unique_ptr<nova::chain::ChainState> state;
    nova::node::SyncParams params;

    void SetUp() override
    {
        params = {{network.block_limits, network.pow, network.monetary, 0U, 500'000'000U,
                   std::numeric_limits<std::uint32_t>::max(), 1U, 600U},
                  network.difficulty,
                  network.protocol_version,
                  2U,
                  2'000U};
        const auto target =
            nova::consensus::TargetFromCompact(network.genesis_block.header.bits, network.pow)
                .target.value();
        const auto work = nova::consensus::CalculateWork(target).value();
        state = nova::chain::ChainState::Create(
            {params.validation, network.difficulty, 11U,
             std::make_shared<nova::chain::FixedValidationTimeSource>(
                 network.genesis_block.header.time + 100'000U)},
            {network.genesis_hash,
             {},
             0U,
             network.genesis_block.header.time,
             target,
             work,
             nova::chain::ChainWork::FromPerBlockWork(work)},
            utxos);
        ASSERT_NE(state, nullptr);
    }

    Block Make(const Hash256& parent, const std::uint32_t height, const std::uint32_t offset = 0U)
    {
        const auto time = network.genesis_block.header.time + height * 600U + offset;
        nova::primitives::Transaction coinbase{
            1,
            {{{{}, 0xFFFF'FFFFU},
              {1U, static_cast<std::uint8_t>(height), static_cast<std::uint8_t>(time & 255U)},
              0U}},
            {{nova::consensus::GetBlockSubsidy(height, network.monetary).value(), {0x51U}}},
            0U};
        Block block{{1, parent, {}, time, network.pow.pow_limit_compact, 0U}, {coinbase}};
        block.header.merkle_root = nova::primitives::ComputeMerkleRoot(
                                       block.transactions, network.block_limits.transaction_limits)
                                       .value();
        const auto mined = nova::consensus::MineRegtestBlock(block.header, 4096U);
        if (!mined.has_value()) {
            throw std::runtime_error{"bounded deterministic mining failed"};
        }
        block.header = mined->header;
        return block;
    }

    void Accept(const Block& block)
    {
        ASSERT_EQ(state->AcceptBlock(block).error, nova::chain::ChainError::kNone);
    }

    nova::node::SyncResult Headers(nova::node::Synchronizer& sync, const std::vector<Block>& blocks)
    {
        nova::net::HeadersMessage message;
        for (const auto& block : blocks) {
            message.headers.push_back(block.header);
        }
        return sync.Handle({nova::net::Command::kHeaders, message});
    }

    std::vector<Hash256> Requests(const nova::node::SyncResult& result)
    {
        EXPECT_EQ(result.error, SyncError::kNone);
        std::vector<Hash256> hashes;
        for (const auto& request : result.requests) {
            EXPECT_EQ(request.command, nova::net::Command::kGetData);
            const auto* inventory = std::get_if<nova::net::InventoryMessage>(&request.message);
            if (inventory == nullptr) {
                ADD_FAILURE() << "missing inventory";
                continue;
            }
            for (const auto& item : inventory->inventory) {
                EXPECT_EQ(item.type, 2U);
                hashes.push_back(item.hash);
            }
        }
        return hashes;
    }

    Hash256 Locator(nova::node::Synchronizer& sync)
    {
        const auto start = sync.Start();
        EXPECT_EQ(start.error, SyncError::kNone);
        const auto& message = std::get<nova::net::GetHeadersMessage>(start.requests.at(0).message);
        EXPECT_EQ(message.locator_hashes.size(), 1U);
        return message.locator_hashes.at(0);
    }
};

TEST_F(SyncProgress, ReconcilesLocalAdvanceAndReorgThenRequestsExactMissingBody)
{
    const auto first = Make(state->active_tip(), 1U);
    Accept(first);
    nova::node::Synchronizer sync{params, *state};
    const auto losing = Make(Hash(first), 2U);
    EXPECT_EQ(Requests(Headers(sync, {losing})), std::vector<Hash256>{Hash(losing)});
    Accept(losing);
    const auto side = Make(Hash(first), 2U, 1U);
    const auto winner = Make(Hash(side), 3U, 1U);
    Accept(side);
    Accept(winner);
    ASSERT_EQ(state->active_tip(), Hash(winner));
    EXPECT_EQ(Locator(sync), Hash(winner));
    EXPECT_EQ(sync.best_header_tip(), Hash(winner));
    const auto next = Make(Hash(winner), 4U, 1U);
    EXPECT_EQ(Requests(Headers(sync, {first, losing, side, winner, next})),
              std::vector<Hash256>{Hash(next)});
    EXPECT_EQ(state->active_tip(), Hash(winner));
    EXPECT_EQ(state->active_height(), 3U);
}

TEST_F(SyncProgress, KeepsHeaderOnlyProgressAndRetriesMissingBodiesAcrossStart)
{
    nova::node::Synchronizer sync{params, *state};
    const auto first = Make(state->active_tip(), 1U);
    const auto second = Make(Hash(first), 2U);
    EXPECT_EQ(Requests(Headers(sync, {first, second})),
              (std::vector<Hash256>{Hash(first), Hash(second)}));
    const auto count = sync.header_count();
    EXPECT_EQ(Locator(sync), network.genesis_hash);
    EXPECT_EQ(sync.best_header_tip(), Hash(second));
    EXPECT_EQ(sync.header_count(), count);
    EXPECT_EQ(Requests(Headers(sync, {first, second})),
              (std::vector<Hash256>{Hash(first), Hash(second)}));
    Accept(first); // ordinary validated body arrived independently
    EXPECT_EQ(Requests(Headers(sync, {first, second})), std::vector<Hash256>{Hash(second)});
    EXPECT_EQ(Locator(sync), Hash(first));
    EXPECT_EQ(sync.best_header_tip(), Hash(second));
}

TEST_F(SyncProgress, DeduplicatesRepeatedBodyRequestsWithinEachResponse)
{
    nova::node::Synchronizer sync{params, *state};
    const auto first = Make(state->active_tip(), 1U);
    const auto second = Make(Hash(first), 2U);
    EXPECT_EQ(Requests(Headers(sync, {first, first, second, first, second})),
              (std::vector<Hash256>{Hash(first), Hash(second)}));
    EXPECT_EQ(sync.header_count(), 3U);
}

TEST_F(SyncProgress, InvalidSuffixRollsBackPrefixAndBestTipThenAllowsRetry)
{
    nova::node::Synchronizer sync{params, *state};
    const auto first = Make(state->active_tip(), 1U);
    const auto second = Make(Hash(first), 2U);
    auto invalid = second;
    invalid.header.bits = 0x0100'3456U;
    const auto count = sync.header_count();
    const auto result = Headers(sync, {first, invalid});
    EXPECT_EQ(result.error, SyncError::kInvalidTarget);
    EXPECT_TRUE(result.requests.empty());
    EXPECT_EQ(sync.header_count(), count);
    EXPECT_EQ(sync.best_header_tip(), network.genesis_hash);
    EXPECT_EQ(Requests(Headers(sync, {first, second})),
              (std::vector<Hash256>{Hash(first), Hash(second)}));
    EXPECT_EQ(state->active_height(), 0U);
}

TEST_F(SyncProgress, PreservesExistingPendingPrefixAfterBadPowAndUnknownParent)
{
    nova::node::Synchronizer sync{params, *state};
    const auto first = Make(state->active_tip(), 1U);
    EXPECT_EQ(Requests(Headers(sync, {first})), std::vector<Hash256>{Hash(first)});
    const auto second = Make(Hash(first), 2U);
    auto bad = second;
    const auto target =
        nova::consensus::TargetFromCompact(bad.header.bits, network.pow).target.value();
    for (std::uint32_t nonce = 0U; nonce < 4096U; ++nonce) {
        bad.header.nonce = nonce;
        if (!nova::consensus::CheckProofOfWork(Hash(bad), target)) {
            break;
        }
    }
    ASSERT_FALSE(nova::consensus::CheckProofOfWork(Hash(bad), target));
    EXPECT_EQ(Headers(sync, {first, bad}).error, SyncError::kBadProofOfWork);
    EXPECT_EQ(sync.header_count(), 2U);
    EXPECT_EQ(sync.best_header_tip(), Hash(first));
    bad.header.previous_block_id = Hash(second);
    EXPECT_EQ(Headers(sync, {first, bad}).error, SyncError::kUnknownHeaderParent);
    EXPECT_EQ(Requests(Headers(sync, {first, second})),
              (std::vector<Hash256>{Hash(first), Hash(second)}));
}

TEST_F(SyncProgress, RejectsOversizedResponseWithoutChangingProgress)
{
    params.max_headers_per_response = 1U;
    nova::node::Synchronizer sync{params, *state};
    const auto first = Make(state->active_tip(), 1U);
    EXPECT_EQ(Headers(sync, {first, first}).error, SyncError::kInvalidParameters);
    EXPECT_EQ(sync.header_count(), 1U);
    EXPECT_EQ(state->active_height(), 0U);
}
} // namespace
