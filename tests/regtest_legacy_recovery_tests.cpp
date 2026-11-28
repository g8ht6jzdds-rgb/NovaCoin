#include "node/mining_helpers.hpp"
#include "node/regtest_node.hpp"

#include "consensus/monetary.hpp"
#include "consensus/pow.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr std::uint32_t kStartTime = 1'704'067'800U;

TEST(CoinbaseHeightEncoding, PreservesMagnitudeAndSignBoundaryBytes)
{
    struct Vector final {
        std::uint32_t height;
        std::vector<std::uint8_t> script;
    };
    for (const auto& item :
         std::vector<Vector>{{0U, {0U}},
                             {1U, {1U, 1U}},
                             {127U, {1U, 0x7fU}},
                             {128U, {2U, 0x80U, 0U}},
                             {255U, {2U, 0xffU, 0U}},
                             {256U, {2U, 0U, 1U}},
                             {32767U, {2U, 0xffU, 0x7fU}},
                             {32768U, {3U, 0U, 0x80U, 0U}},
                             {65535U, {3U, 0xffU, 0xffU, 0U}},
                             {65536U, {3U, 0U, 0U, 1U}},
                             {0x7fffffU, {3U, 0xffU, 0xffU, 0x7fU}},
                             {0x800000U, {4U, 0U, 0U, 0x80U, 0U}},
                             {0xffffffU, {4U, 0xffU, 0xffU, 0xffU, 0U}},
                             {0x1000000U, {4U, 0U, 0U, 0U, 1U}},
                             {0x7fffffffU, {4U, 0xffU, 0xffU, 0xffU, 0x7fU}},
                             {0x80000000U, {5U, 0U, 0U, 0U, 0x80U, 0U}},
                             {0xffffffffU, {5U, 0xffU, 0xffU, 0xffU, 0xffU, 0U}}}) {
        SCOPED_TRACE(item.height);
        EXPECT_EQ(nova::node::CoinbaseHeightScript(item.height), item.script);
    }
}

std::optional<nova::primitives::Block> BranchBlock(const nova::crypto::Hash256& parent,
                                                   const std::uint32_t height,
                                                   const std::uint8_t branch)
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto subsidy = nova::consensus::GetBlockSubsidy(height, network.monetary);
    if (!subsidy || height > 127U)
        return std::nullopt;
    nova::primitives::Transaction coinbase{
        1,
        {{nova::primitives::OutPoint{nova::crypto::Hash256{},
                                     std::numeric_limits<std::uint32_t>::max()},
          {1U, static_cast<std::uint8_t>(height), branch},
          0U}},
        {{*subsidy, {0x51U}}},
        0U};
    nova::primitives::Block block{
        {1, parent, {}, kStartTime + height, network.pow.pow_limit_compact, 0U},
        {std::move(coinbase)}};
    const auto merkle = nova::primitives::ComputeMerkleRoot(
        block.transactions, network.block_limits.transaction_limits);
    if (!merkle)
        return std::nullopt;
    block.header.merkle_root = *merkle;
    const auto mined = nova::consensus::MineRegtestBlock(block.header, 4096U);
    if (!mined)
        return std::nullopt;
    block.header = mined->header;
    return block;
}

TEST(RegtestLegacyRecovery, ValidInactiveAncestryMustSurviveRestart)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-inactive-ancestry-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    SCOPED_TRACE(root.string());
    const nova::node::RegtestNodeConfig config{"ancestry",
                                               root / "data",
                                               root / "logs" / "node.log",
                                               29201U,
                                               29202U,
                                               kStartTime,
                                               {'r', 'e', 'g', 't', 'e', 's', 't'},
                                               nova::consensus::NetworkId::kRegtest};
    const auto& network = nova::consensus::RegtestNetworkParams();
    auto node = nova::node::RegtestNode::Create(config);
    ASSERT_NE(node, nullptr);
    const auto a1 = BranchBlock(network.genesis_hash, 1U, 0x41U);
    ASSERT_TRUE(a1);
    const auto a1_hash = nova::primitives::ComputeBlockHash(a1->header);
    ASSERT_TRUE(a1_hash);
    const auto a2 = BranchBlock(*a1_hash, 2U, 0x41U);
    ASSERT_TRUE(a2);
    const auto a2_hash = nova::primitives::ComputeBlockHash(a2->header);
    ASSERT_TRUE(a2_hash);
    const auto b1 = BranchBlock(network.genesis_hash, 1U, 0x42U);
    ASSERT_TRUE(b1);
    const auto b1_hash = nova::primitives::ComputeBlockHash(b1->header);
    ASSERT_TRUE(b1_hash);
    const auto b2 = BranchBlock(*b1_hash, 2U, 0x42U);
    ASSERT_TRUE(b2);
    const auto b2_hash = nova::primitives::ComputeBlockHash(b2->header);
    ASSERT_TRUE(b2_hash);
    const auto b3 = BranchBlock(*b2_hash, 3U, 0x42U);
    ASSERT_TRUE(b3);
    const auto b3_hash = nova::primitives::ComputeBlockHash(b3->header);
    ASSERT_TRUE(b3_hash);
    for (const auto* block : {&*a1, &*a2, &*b1})
        ASSERT_EQ(node->ReceiveBlock(*block), nova::node::RegtestNodeError::kNone);
    // This ancestor is strictly lower-work, so the test does not depend on ties.
    ASSERT_EQ(node->tip(), *a2_hash);
    ASSERT_EQ(node->ReceiveBlock(*b2), nova::node::RegtestNodeError::kNone);
    ASSERT_EQ(node->ReceiveBlock(*b3), nova::node::RegtestNodeError::kNone);
    ASSERT_EQ(node->tip(), *b3_hash);
    const auto tip_index = node->chain_state().GetBlockIndex(*b3_hash);
    ASSERT_TRUE(tip_index);
    const auto work = tip_index->chain_work;
    auto invalid = BranchBlock(*b3_hash, 4U, 0x42U);
    ASSERT_TRUE(invalid);
    ++invalid->transactions.front().outputs.front().value;
    const auto invalid_merkle = nova::primitives::ComputeMerkleRoot(
        invalid->transactions, network.block_limits.transaction_limits);
    ASSERT_TRUE(invalid_merkle);
    invalid->header.merkle_root = *invalid_merkle;
    invalid->header.nonce = 0U;
    const auto invalid_mined = nova::consensus::MineRegtestBlock(invalid->header, 4096U);
    ASSERT_TRUE(invalid_mined);
    invalid->header = invalid_mined->header;
    {
        auto before = node->utxos().BeginBatch();
        ASSERT_TRUE(before);
        const auto wallet_before = node->wallet().CaptureCheckpoint();
        EXPECT_EQ(node->ReceiveBlock(*invalid), nova::node::RegtestNodeError::kBlockRejected);
        EXPECT_TRUE(before->IsCurrent());
        EXPECT_TRUE(node->wallet().IsCheckpointCurrent(wallet_before));
        EXPECT_EQ(node->tip(), *b3_hash);
        const auto after = node->chain_state().GetBlockIndex(*b3_hash);
        ASSERT_TRUE(after);
        EXPECT_EQ(after->chain_work, work);
        EXPECT_EQ(node->utxos().size(), 3U);
        EXPECT_EQ(node->mempool_size(), 0U);
        for (const auto* block : {&*b1, &*b2, &*b3}) {
            const auto id =
                block->transactions.front().TxId(network.block_limits.transaction_limits);
            ASSERT_TRUE(id);
            const auto coin = node->utxos().GetCoin({*id, 0U});
            ASSERT_TRUE(coin);
            EXPECT_EQ(coin->output.value, block->transactions.front().outputs.front().value);
            EXPECT_EQ(coin->output.script_pubkey,
                      block->transactions.front().outputs.front().script_pubkey);
            EXPECT_TRUE(coin->is_coinbase);
        }
    }
    node.reset();
    {
        std::ifstream journal{config.data_directory / "blocks.dat", std::ios::binary};
        const auto saved = nova::storage::ReadBlockJournal(journal, network.block_limits);
        ASSERT_EQ(saved.error, nova::storage::BlockJournalError::kNone);
        EXPECT_EQ(saved.blocks.size(), 5U);
    }
    for (unsigned restart = 0U; restart < 2U; ++restart) {
        node = nova::node::RegtestNode::Create(config);
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->tip(), *b3_hash);
        EXPECT_EQ(node->height(), 3U);
        const auto restored = node->chain_state().GetBlockIndex(*b3_hash);
        ASSERT_TRUE(restored);
        EXPECT_EQ(restored->chain_work, work);
        EXPECT_EQ(node->utxos().size(), 3U);
        EXPECT_EQ(node->wallet().ConfirmedBalance(), 0);
        EXPECT_EQ(node->mempool_size(), 0U);
        for (const auto* block : {&*a1, &*a2, &*b1, &*b2, &*b3}) {
            const auto hash = nova::primitives::ComputeBlockHash(block->header);
            ASSERT_TRUE(hash);
            EXPECT_TRUE(node->chain_state().GetBlockIndex(*hash));
            const auto id =
                block->transactions.front().TxId(network.block_limits.transaction_limits);
            ASSERT_TRUE(id);
            const auto coin = node->utxos().GetCoin({*id, 0U});
            const bool active = block == &*b1 || block == &*b2 || block == &*b3;
            ASSERT_EQ(coin.has_value(), active);
            if (coin) {
                EXPECT_EQ(coin->output.value, block->transactions.front().outputs.front().value);
                EXPECT_EQ(coin->output.script_pubkey,
                          block->transactions.front().outputs.front().script_pubkey);
                EXPECT_EQ(coin->height, block == &*b1 ? 1U : block == &*b2 ? 2U : 3U);
                EXPECT_TRUE(coin->is_coinbase);
            }
        }
        node.reset();
    }
    if (HasFailure())
        return; // Retain the fixture named by SCOPED_TRACE on any failure.
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}
} // namespace
