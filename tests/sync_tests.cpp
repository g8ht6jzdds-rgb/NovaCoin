#include <gtest/gtest.h>

#include "chain/network_chainstate.hpp"
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
                  2'000U,
                  4'096U};
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

TEST_F(SyncProgress, RetargetContextRequiresPositiveQuarterTimespan)
{
    const auto tip = state->active_tip();
    for (const auto no_retargeting : {false, true}) {
        for (std::uint32_t timespan = 0U; timespan <= 4U; ++timespan) {
            SCOPED_TRACE(timespan);
            SCOPED_TRACE(no_retargeting);
            auto controlled = params;
            controlled.difficulty = {timespan, 1U, timespan, false, no_retargeting};
            nova::node::Synchronizer sync{controlled, *state};
            const auto result = Headers(sync, {});
            const auto admitted = timespan != 0U && (no_retargeting || timespan >= 4U);
            EXPECT_EQ(result.error, admitted ? SyncError::kNone : SyncError::kInvalidParameters);
            EXPECT_TRUE(result.requests.empty());
            EXPECT_EQ(sync.header_count(), 0U);
            EXPECT_EQ(sync.best_header_tip(), tip);
            EXPECT_EQ(state->active_tip(), tip);
            EXPECT_EQ(state->active_height(), 0U);
            EXPECT_EQ(utxos.size(), 0U);
        }
    }
}

TEST_F(SyncProgress, NoRetargetingInheritsValidatedAndPendingParentBits)
{
    // In-memory anchors distinguish inheritance from a powLimit fallback.
    // No immutable network or genesis fixture is changed by these anchors.
    for (const auto bits : {0x206F'FFFFU, 0x2070'FFFEU, network.pow.pow_limit_compact}) {
        SCOPED_TRACE(bits);
        const auto target = nova::consensus::TargetFromCompact(bits, network.pow);
        ASSERT_TRUE(target.target.has_value());
        const auto work = nova::consensus::CalculateWork(*target.target);
        ASSERT_TRUE(work.has_value());
        Hash256::Bytes anchor_bytes{};
        anchor_bytes.back() = 0xA7U;
        const Hash256 anchor_hash{anchor_bytes};
        nova::chain::UTXOSet controlled_utxos;
        auto controlled = nova::chain::ChainState::Create(
            {params.validation, network.difficulty, 11U,
             std::make_shared<nova::chain::FixedValidationTimeSource>(
                 network.genesis_block.header.time + 100'000U)},
            {anchor_hash,
             {},
             0U,
             network.genesis_block.header.time,
             *target.target,
             *work,
             nova::chain::ChainWork::FromPerBlockWork(*work)},
            controlled_utxos);
        ASSERT_NE(controlled, nullptr);
        nova::node::Synchronizer sync{params, *controlled};

        auto wrong = Make(anchor_hash, 1U);
        wrong.header.bits =
            bits == network.pow.pow_limit_compact ? 0x206F'FFFFU : network.pow.pow_limit_compact;
        const auto rejected = Headers(sync, {wrong});
        EXPECT_EQ(rejected.error, SyncError::kInvalidTarget);
        EXPECT_TRUE(rejected.requests.empty());
        EXPECT_EQ(sync.header_count(), 0U);
        EXPECT_EQ(sync.best_header_tip(), anchor_hash);

        const auto inherited = [&](const Hash256& parent, const std::uint32_t height) {
            auto block = Make(parent, height);
            block.header.bits = bits;
            for (std::uint32_t nonce = 0U; nonce < 4096U; ++nonce) {
                block.header.nonce = nonce;
                if (nova::consensus::CheckProofOfWork(Hash(block), *target.target)) {
                    return block;
                }
            }
            throw std::runtime_error{"bounded inherited-target mining failed"};
        };
        const auto first = inherited(anchor_hash, 1U);
        EXPECT_EQ(Requests(Headers(sync, {first})), (std::vector<Hash256>{Hash(first)}));
        const auto second = inherited(Hash(first), 2U);
        EXPECT_EQ(Headers(sync, {second}).error, SyncError::kNone);
        EXPECT_EQ(sync.header_count(), 2U);
        EXPECT_EQ(sync.best_header_tip(), Hash(second));
        EXPECT_EQ(controlled->active_tip(), anchor_hash);
        EXPECT_EQ(controlled->active_height(), 0U);
        EXPECT_EQ(controlled_utxos.size(), 0U);
    }
}

TEST_F(SyncProgress, MissingTestnetRecoveryContextRollsBackPendingBatch)
{
    const auto& testnet = nova::consensus::TestnetNetworkParams();
    const auto target =
        nova::consensus::TargetFromCompact(testnet.pow.pow_limit_compact, testnet.pow);
    ASSERT_TRUE(target.target.has_value());
    const auto work = nova::consensus::CalculateWork(*target.target);
    ASSERT_TRUE(work.has_value());
    Hash256::Bytes anchor_bytes{};
    anchor_bytes.back() = 0xB9U;
    const Hash256 anchor_hash{anchor_bytes};
    const auto anchor_time =
        testnet.genesis_block.header.time + testnet.difficulty.target_spacing_seconds;
    auto controlled_params = params;
    controlled_params.validation = nova::chain::NetworkValidationParams(testnet);
    controlled_params.difficulty = testnet.difficulty;
    nova::chain::UTXOSet controlled_utxos;
    auto controlled = nova::chain::ChainState::Create(
        {controlled_params.validation, testnet.difficulty, 11U,
         std::make_shared<nova::chain::FixedValidationTimeSource>(anchor_time + 100'000U)},
        {anchor_hash,
         {},
         1U,
         anchor_time,
         *target.target,
         *work,
         nova::chain::ChainWork::FromPerBlockWork(*work)},
        controlled_utxos);
    ASSERT_NE(controlled, nullptr);
    nova::node::Synchronizer sync{controlled_params, *controlled};
    const auto header = [&](const Hash256& parent, const std::uint32_t height,
                            const std::uint32_t time) {
        auto block = Make(parent, height);
        block.header.time = time;
        block.header.bits = testnet.pow.pow_limit_compact;
        for (std::uint32_t nonce = 0U; nonce < 4096U; ++nonce) {
            block.header.nonce = nonce;
            if (nova::consensus::CheckProofOfWork(Hash(block), *target.target))
                return block;
        }
        throw std::runtime_error{"bounded context-fixture header mining failed"};
    };
    const auto spacing = testnet.difficulty.target_spacing_seconds;
    for (const auto delay : {spacing, spacing * 2U}) {
        const auto result = Headers(sync, {header(anchor_hash, 2U, anchor_time + delay)});
        EXPECT_EQ(result.error, SyncError::kChainStateUnavailable);
        EXPECT_TRUE(result.requests.empty());
        EXPECT_EQ(sync.header_count(), 0U);
        EXPECT_EQ(sync.best_header_tip(), anchor_hash);
    }
    const auto delayed = header(anchor_hash, 2U, anchor_time + spacing * 2U + 1U);
    const auto recovery = header(Hash(delayed), 3U, delayed.header.time + spacing);
    const auto batch = Headers(sync, {delayed, recovery});
    EXPECT_EQ(batch.error, SyncError::kChainStateUnavailable);
    EXPECT_TRUE(batch.requests.empty());
    EXPECT_EQ(sync.header_count(), 0U);
    EXPECT_EQ(sync.best_header_tip(), anchor_hash);
    EXPECT_EQ(Requests(Headers(sync, {delayed})), (std::vector<Hash256>{Hash(delayed)}));
    EXPECT_EQ(Headers(sync, {recovery}).error, SyncError::kChainStateUnavailable);
    EXPECT_EQ(sync.header_count(), 1U);
    EXPECT_EQ(sync.best_header_tip(), Hash(delayed));
    EXPECT_EQ(controlled->active_tip(), anchor_hash);
    EXPECT_EQ(controlled->active_height(), 1U);
    EXPECT_EQ(controlled_utxos.size(), 0U);
}

TEST_F(SyncProgress, SelfLinkedTestnetDifficultyAncestryIsUnavailableContext)
{
    const auto& testnet = nova::consensus::TestnetNetworkParams();
    const auto target =
        nova::consensus::TargetFromCompact(testnet.pow.pow_limit_compact, testnet.pow);
    ASSERT_TRUE(target.target.has_value());
    const auto work = nova::consensus::CalculateWork(*target.target);
    ASSERT_TRUE(work.has_value());
    Hash256::Bytes anchor_bytes{};
    anchor_bytes.back() = 0xB9U;
    const Hash256 anchor_hash{anchor_bytes};
    const auto anchor_time =
        testnet.genesis_block.header.time + testnet.difficulty.target_spacing_seconds;
    auto controlled_params = params;
    controlled_params.validation = nova::chain::NetworkValidationParams(testnet);
    controlled_params.difficulty = testnet.difficulty;
    for (const auto anchor_height : {1U, testnet.difficulty.retarget_interval - 1U}) {
        nova::chain::UTXOSet controlled_utxos;
        auto controlled = nova::chain::ChainState::Create(
            {controlled_params.validation, testnet.difficulty, 11U,
             std::make_shared<nova::chain::FixedValidationTimeSource>(anchor_time + 100'000U)},
            {anchor_hash, anchor_hash, anchor_height, anchor_time, *target.target, *work,
             nova::chain::ChainWork::FromPerBlockWork(*work)},
            controlled_utxos);
        ASSERT_NE(controlled, nullptr);
        nova::node::Synchronizer sync{controlled_params, *controlled};
        const auto header = [&](const Hash256& parent, const std::uint32_t height,
                                const std::uint32_t time) {
            auto block = Make(parent, height);
            block.header.time = time;
            block.header.bits = testnet.pow.pow_limit_compact;
            for (std::uint32_t nonce = 0U; nonce < 4096U; ++nonce) {
                block.header.nonce = nonce;
                if (nova::consensus::CheckProofOfWork(Hash(block), *target.target))
                    return block;
            }
            throw std::runtime_error{"bounded context-fixture header mining failed"};
        };
        const auto spacing = testnet.difficulty.target_spacing_seconds;
        for (const auto delay : {spacing, spacing * 2U}) {
            const auto result =
                Headers(sync, {header(anchor_hash, anchor_height + 1U, anchor_time + delay)});
            EXPECT_EQ(result.error, SyncError::kChainStateUnavailable);
            EXPECT_TRUE(result.requests.empty());
            EXPECT_EQ(sync.header_count(), 0U);
            EXPECT_EQ(sync.best_header_tip(), anchor_hash);
        }
        EXPECT_EQ(controlled->active_tip(), anchor_hash);
        EXPECT_EQ(controlled_utxos.size(), 0U);
    }
}

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
    EXPECT_EQ(sync.header_count(), 2U);
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
    EXPECT_EQ(sync.header_count(), 1U);
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
    EXPECT_EQ(sync.header_count(), 0U);
    EXPECT_EQ(state->active_height(), 0U);
}
TEST_F(SyncProgress, SaturationRollsBackWholeBatchPreservingPendingWork)
{
    params.max_pending_headers = 2U;
    nova::node::Synchronizer sync{params, *state};
    const auto first = Make(state->active_tip(), 1U);
    const auto second = Make(Hash(first), 2U);
    const auto third = Make(Hash(second), 3U);
    EXPECT_EQ(Requests(Headers(sync, {first})), std::vector<Hash256>{Hash(first)});
    const auto best = sync.best_header_tip();
    for (unsigned int retry = 0U; retry < 4U; ++retry) {
        const auto result = Headers(sync, {second, third});
        EXPECT_EQ(result.error, SyncError::kPendingHeaderLimit);
        EXPECT_TRUE(result.requests.empty());
        EXPECT_EQ(sync.header_count(), 1U);
        EXPECT_EQ(sync.best_header_tip(), best);
    }
    EXPECT_EQ(Requests(Headers(sync, {first, second})),
              (std::vector<Hash256>{Hash(first), Hash(second)}));
    EXPECT_EQ(sync.header_count(), 2U);
    EXPECT_EQ(Locator(sync), network.genesis_hash);
    EXPECT_EQ(Requests(Headers(sync, {second, first, second})),
              (std::vector<Hash256>{Hash(second), Hash(first)}));
    EXPECT_EQ(Headers(sync, {third}).error, SyncError::kPendingHeaderLimit);
    Accept(first); // actual authoritative delivery, outside Synchronizer
    EXPECT_EQ(Requests(Headers(sync, {second, third})),
              (std::vector<Hash256>{Hash(second), Hash(third)}));
    EXPECT_EQ(sync.header_count(), 2U);
    EXPECT_EQ(Locator(sync), Hash(first));
    EXPECT_EQ(sync.best_header_tip(), Hash(third));
}

TEST_F(SyncProgress, ValidatedChainBeyondQuotaUsesNoPendingSlots)
{
    params.max_pending_headers = 1U;
    auto tip = state->active_tip();
    for (std::uint32_t height = 1U; height <= 8U; ++height) {
        const auto block = Make(tip, height);
        Accept(block);
        tip = Hash(block);
    }
    nova::node::Synchronizer sync{params, *state};
    EXPECT_EQ(sync.header_count(), 0U);
    EXPECT_EQ(Locator(sync), tip);
    const auto ninth = Make(tip, 9U);
    EXPECT_EQ(Requests(Headers(sync, {ninth})), std::vector<Hash256>{Hash(ninth)});
    Accept(ninth);
    const auto tenth = Make(Hash(ninth), 10U);
    EXPECT_EQ(Requests(Headers(sync, {ninth, tenth})), std::vector<Hash256>{Hash(tenth)});
    EXPECT_EQ(sync.header_count(), 1U);
    EXPECT_EQ(state->active_height(), 9U);
    const auto metadata = state->GetHeaderMetadata(Hash(ninth));
    ASSERT_TRUE(metadata.has_value());
    const auto copied = state->GetBlockIndex(Hash(ninth));
    ASSERT_TRUE(copied.has_value());
    EXPECT_EQ(metadata->chain_work, copied->chain_work);
    EXPECT_EQ(metadata->bits, ninth.header.bits);
    EXPECT_FALSE(state->GetHeaderMetadata(Hash(tenth)).has_value());
}

TEST_F(SyncProgress, InvalidBodiesCannotReclaimCapacity)
{
    params.max_pending_headers = 1U;
    nova::node::Synchronizer sync{params, *state};
    const auto first = Make(state->active_tip(), 1U);
    const auto second = Make(Hash(first), 2U);
    EXPECT_EQ(Requests(Headers(sync, {first})), std::vector<Hash256>{Hash(first)});
    auto invalid = first;
    invalid.transactions.clear();
    EXPECT_NE(state->AcceptBlock(invalid).error, nova::chain::ChainError::kNone);
    EXPECT_EQ(Headers(sync, {second}).error, SyncError::kPendingHeaderLimit);
    EXPECT_EQ(sync.header_count(), 1U);
    EXPECT_EQ(Requests(Headers(sync, {first})), std::vector<Hash256>{Hash(first)});
}

TEST_F(SyncProgress, RejectsInvalidResourceConfiguration)
{
    for (const auto cap : {0U, 65'537U, std::numeric_limits<std::uint32_t>::max()}) {
        params.max_pending_headers = cap;
        nova::node::Synchronizer sync{params, *state};
        EXPECT_EQ(sync.Start().error, SyncError::kInvalidParameters);
        EXPECT_EQ(sync.header_count(), 0U);
    }
}

TEST_F(SyncProgress, RetargetWalkSpansValidatedAndPendingMetadata)
{
    const auto first = Make(state->active_tip(), 1U);
    const auto second = Make(Hash(first), 2U);
    Accept(first);
    Accept(second);
    params.max_pending_headers = 4U;
    params.difficulty = {600U, 4U, 2400U, true, false};
    nova::node::Synchronizer sync{params, *state};
    const auto third = Make(Hash(second), 3U);
    auto fourth = Make(Hash(third), 4U);
    // Three 600s intervals from the anchor to the parent: 3/4 powLimit.
    fourth.header.bits = 0x205F'FFFFU;
    const auto target = nova::consensus::TargetFromCompact(fourth.header.bits, network.pow);
    ASSERT_TRUE(target.target.has_value());
    bool mined{};
    for (std::uint32_t nonce = 0U; nonce < 4096U; ++nonce) {
        fourth.header.nonce = nonce;
        if (nova::consensus::CheckProofOfWork(Hash(fourth), *target.target)) {
            mined = true;
            break;
        }
    }
    ASSERT_TRUE(mined);
    EXPECT_EQ(Requests(Headers(sync, {third, fourth})),
              (std::vector<Hash256>{Hash(third), Hash(fourth)}));
    EXPECT_EQ(sync.header_count(), 2U);
    EXPECT_EQ(state->active_tip(), Hash(second));
    EXPECT_EQ(Locator(sync), Hash(second));
    // Reclaim the pending ancestor through actual ChainState acceptance, then
    // recover pre-delay difficulty across a pending/validated ancestry split.
    Accept(third);
    auto fifth = Make(Hash(fourth), 5U, 601U);
    fifth.header.time = fourth.header.time + 1201U;
    fifth.header = nova::consensus::MineRegtestBlock(fifth.header, 4096U).value().header;
    auto sixth = Make(Hash(fifth), 6U, 601U);
    sixth.header.bits = fourth.header.bits;
    mined = false;
    for (std::uint32_t nonce = 0U; nonce < 4096U; ++nonce) {
        sixth.header.nonce = nonce;
        if (nova::consensus::CheckProofOfWork(Hash(sixth), *target.target)) {
            mined = true;
            break;
        }
    }
    ASSERT_TRUE(mined);
    EXPECT_EQ(Requests(Headers(sync, {fourth, fifth, sixth})),
              (std::vector<Hash256>{Hash(fourth), Hash(fifth), Hash(sixth)}));
    EXPECT_EQ(sync.header_count(), 3U);
    EXPECT_EQ(state->active_tip(), Hash(third));
}
} // namespace
