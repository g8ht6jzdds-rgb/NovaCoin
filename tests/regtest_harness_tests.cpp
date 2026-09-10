#include <gtest/gtest.h>

#include "node/regtest_node.hpp"

#include "consensus/monetary.hpp"
#include "consensus/network_params.hpp"
#include "consensus/pow.hpp"
#include "primitives/block.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{

using nova::node::RegtestNode;
using nova::node::RegtestNodeConfig;
using nova::node::RegtestNodeError;

constexpr std::uint32_t kStartTime = 1'704'067'800U;

RegtestNodeConfig Config(const std::filesystem::path& root, const std::string_view name,
                         const std::uint16_t p2p_port, const std::uint16_t rpc_port)
{
    const auto node_root = root / std::string{name};
    return {std::string{name},
            node_root / "data",
            node_root / "logs" / "novacoind.log",
            p2p_port,
            rpc_port,
            kStartTime,
            {'r', 'e', 'g', 't', 'e', 's', 't'},
            nova::consensus::NetworkId::kRegtest};
}

std::vector<std::uint8_t> CoinbaseHeightScript(const std::uint32_t height)
{
    return {0x01U, static_cast<std::uint8_t>(height)};
}

std::vector<std::uint8_t> P2pkhScript(const nova::crypto::Hash160& hash)
{
    std::vector<std::uint8_t> script{0x76U, 0xA9U, 0x14U};
    script.insert(script.end(), hash.begin(), hash.end());
    script.insert(script.end(), {0x88U, 0xACU});
    return script;
}

std::optional<nova::primitives::Block> FarFutureBlock(RegtestNode& node)
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto address = node.wallet().GenerateReceivingAddress();
    const auto subsidy = nova::consensus::GetBlockSubsidy(1U, network.monetary);
    if (!address.has_value() || !subsidy.has_value()) {
        return std::nullopt;
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (now < 0 ||
        static_cast<std::uint64_t>(now) > std::numeric_limits<std::uint32_t>::max() - 1'000U) {
        return std::nullopt;
    }
    nova::primitives::Transaction coinbase{
        1,
        {{nova::primitives::OutPoint{nova::crypto::Hash256{}, 0xFFFF'FFFFU},
          CoinbaseHeightScript(1U), 0U}},
        {{*subsidy, P2pkhScript(address->public_key_hash)}},
        0U};
    nova::primitives::Block block{{1, node.tip(), nova::crypto::Hash256{},
                                   static_cast<std::uint32_t>(now) + 1'000U,
                                   network.pow.pow_limit_compact, 0U},
                                  {std::move(coinbase)}};
    const auto root = nova::primitives::ComputeMerkleRoot(block.transactions,
                                                          network.block_limits.transaction_limits);
    if (!root.has_value()) {
        return std::nullopt;
    }
    block.header.merkle_root = *root;
    const auto mined = nova::consensus::MineRegtestBlock(block.header, 4'096U);
    if (!mined.has_value()) {
        return std::nullopt;
    }
    block.header = mined->header;
    return block;
}

TEST(RegtestHarness, ExercisesFourNodeLifecycleAndAdversarialPaths)
{
    const auto root = std::filesystem::temp_directory_path() / "novacoin-regtest-harness";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_FALSE(error);

    const auto alpha_config = Config(root, "alpha", 18444U, 18443U);
    const auto bravo_config = Config(root, "bravo", 18445U, 18442U);
    const auto charlie_config = Config(root, "charlie", 18446U, 18441U);
    const auto delta_config = Config(root, "delta", 18447U, 18440U);
    auto alpha = RegtestNode::Create(alpha_config);
    auto bravo = RegtestNode::Create(bravo_config);
    auto charlie = RegtestNode::Create(charlie_config);
    auto delta = RegtestNode::Create(delta_config);
    ASSERT_NE(alpha, nullptr);
    ASSERT_NE(bravo, nullptr);
    ASSERT_NE(charlie, nullptr);
    ASSERT_NE(delta, nullptr);
    EXPECT_NE(alpha->config().data_directory, bravo->config().data_directory);
    EXPECT_NE(alpha->config().p2p_port, bravo->config().p2p_port);
    EXPECT_NE(alpha->config().rpc_port, bravo->config().rpc_port);
    EXPECT_TRUE(std::filesystem::exists(alpha->config().log_path));
    EXPECT_TRUE(std::filesystem::exists(bravo->config().data_directory / "wallet.dat"));

    ASSERT_EQ(alpha->MineBlock().error, RegtestNodeError::kNone);
    ASSERT_EQ(alpha->height(), 1U);
    ASSERT_TRUE(alpha->Connect(*bravo));
    ASSERT_TRUE(bravo->Connect(*charlie));
    ASSERT_TRUE(charlie->Connect(*delta));
    EXPECT_EQ(bravo->height(), 1U);
    EXPECT_EQ(charlie->height(), 1U);
    EXPECT_EQ(delta->height(), 1U);

    ASSERT_EQ(alpha->MineBlock().error, RegtestNodeError::kNone);
    EXPECT_EQ(alpha->height(), 2U);
    EXPECT_EQ(bravo->height(), 2U);
    EXPECT_EQ(charlie->height(), 2U);
    EXPECT_EQ(delta->height(), 2U);

    const auto recipient = bravo->wallet().GenerateReceivingAddress();
    ASSERT_TRUE(recipient.has_value());
    const std::array<nova::wallet::Recipient, 1U> recipients{
        nova::wallet::Recipient{recipient->public_key_hash, nova::consensus::COIN}};
    const auto spend = alpha->CreateAndBroadcast(recipients);
    ASSERT_EQ(spend.error, RegtestNodeError::kNone);
    EXPECT_EQ(alpha->mempool_size(), 1U);
    EXPECT_EQ(bravo->mempool_size(), 1U);
    ASSERT_EQ(bravo->MineBlock().error, RegtestNodeError::kNone);
    EXPECT_EQ(bravo->height(), 3U);
    EXPECT_EQ(bravo->mempool_size(), 0U);
    EXPECT_GT(bravo->wallet().ConfirmedBalance(), 0);

    bravo->DisconnectAll();
    const auto change_address = alpha->wallet().ListReceivingAddresses().front();
    const std::array<nova::wallet::Recipient, 1U> reorg_recipients{
        nova::wallet::Recipient{change_address.public_key_hash, nova::consensus::COIN}};
    ASSERT_EQ(alpha->CreateAndBroadcast(reorg_recipients).error, RegtestNodeError::kNone);
    EXPECT_EQ(alpha->mempool_size(), 1U);
    ASSERT_EQ(alpha->MineBlock().error, RegtestNodeError::kNone);
    ASSERT_EQ(charlie->MineBlock().error, RegtestNodeError::kNone);
    ASSERT_EQ(charlie->MineBlock().error, RegtestNodeError::kNone);
    ASSERT_TRUE(alpha->Connect(*charlie));
    EXPECT_EQ(alpha->height(), charlie->height());
    EXPECT_EQ(alpha->tip(), charlie->tip());
    EXPECT_EQ(alpha->mempool_size(), 1U);

    alpha->DisconnectAll();
    const auto persisted_height = alpha->height();
    const auto persisted_tip = alpha->tip();
    alpha.reset();
    alpha = RegtestNode::Create(alpha_config);
    ASSERT_NE(alpha, nullptr);
    EXPECT_EQ(alpha->height(), persisted_height);
    EXPECT_EQ(alpha->tip(), persisted_tip);

    // A crash after the first byte of a journal record leaves only an
    // incomplete final write. Recovery must retain the committed prefix.
    alpha.reset();
    {
        std::ofstream journal{alpha_config.data_directory / "blocks.dat",
                              std::ios::binary | std::ios::app};
        ASSERT_TRUE(journal.is_open());
        journal.put(static_cast<char>(1));
    }
    alpha = RegtestNode::Create(alpha_config);
    ASSERT_NE(alpha, nullptr);
    EXPECT_EQ(alpha->height(), persisted_height);
    EXPECT_EQ(alpha->tip(), persisted_tip);

    // Recovery is not complete until NEW commits survive another restart.
    ASSERT_EQ(alpha->MineBlock().error, RegtestNodeError::kNone);
    ASSERT_EQ(alpha->MineBlock().error, RegtestNodeError::kNone);
    const auto recovered_tip = alpha->tip();
    const auto recovered_height = alpha->height();
    const auto recovered_index = alpha->chain_state().GetBlockIndex(recovered_tip);
    ASSERT_TRUE(recovered_index.has_value());
    const auto recovered_coins = alpha->wallet().ListUTXOs();
    const auto recovered_balance = alpha->wallet().ConfirmedBalance();
    const auto recovered_utxo_count = alpha->utxos().size();
    std::map<nova::chain::UTXOKey, nova::chain::Coin> recovered_utxos;
    const auto active_blocks = alpha->chain_state().GetActiveBlockIndexes();
    ASSERT_TRUE(active_blocks.has_value());
    for (const auto& entry : *active_blocks) {
        if (!entry.block.has_value()) {
            continue;
        }
        for (const auto& transaction : entry.block->transactions) {
            const auto txid = transaction.TxId(
                nova::consensus::RegtestNetworkParams().block_limits.transaction_limits);
            ASSERT_TRUE(txid.has_value());
            for (std::size_t index = 0; index < transaction.outputs.size(); ++index) {
                const nova::chain::UTXOKey key{*txid, static_cast<std::uint32_t>(index)};
                const auto coin = alpha->utxos().GetCoin(key);
                if (coin.has_value()) {
                    recovered_utxos.emplace(key, *coin);
                }
            }
        }
    }
    ASSERT_EQ(recovered_utxos.size(), recovered_utxo_count);
    alpha.reset();
    alpha = RegtestNode::Create(alpha_config);
    ASSERT_NE(alpha, nullptr);
    EXPECT_EQ(alpha->tip(), recovered_tip);
    EXPECT_EQ(alpha->height(), recovered_height);
    const auto restored_index = alpha->chain_state().GetBlockIndex(alpha->tip());
    ASSERT_TRUE(restored_index.has_value());
    EXPECT_EQ(restored_index->chain_work, recovered_index->chain_work);
    EXPECT_EQ(alpha->wallet().ConfirmedBalance(), recovered_balance);
    EXPECT_EQ(alpha->utxos().size(), recovered_utxo_count);
    for (const auto& [key, expected] : recovered_utxos) {
        const auto actual = alpha->utxos().GetCoin(key);
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(actual->output.value, expected.output.value);
        EXPECT_EQ(actual->output.script_pubkey, expected.output.script_pubkey);
        EXPECT_EQ(actual->height, expected.height);
        EXPECT_EQ(actual->is_coinbase, expected.is_coinbase);
    }
    const auto restored_coins = alpha->wallet().ListUTXOs();
    ASSERT_EQ(restored_coins.size(), recovered_coins.size());
    for (std::size_t index = 0; index < restored_coins.size(); ++index) {
        const auto& expected = recovered_coins[index];
        const auto& actual = restored_coins[index];
        EXPECT_EQ(actual.key, expected.key);
        EXPECT_EQ(actual.output.value, expected.output.value);
        EXPECT_EQ(actual.output.script_pubkey, expected.output.script_pubkey);
        EXPECT_EQ(actual.height, expected.height);
        EXPECT_EQ(actual.confirmed, expected.confirmed);
        const auto coin = alpha->utxos().GetCoin(expected.key);
        ASSERT_TRUE(coin.has_value());
        EXPECT_EQ(coin->output.value, expected.output.value);
    }

    const nova::primitives::Block invalid_block{};
    EXPECT_EQ(delta->ReceiveBlock(invalid_block), RegtestNodeError::kBlockRejected);
    const nova::primitives::Transaction invalid_transaction{1, {}, {{1, {0x51U}}}, 0U};
    EXPECT_EQ(delta->ReceiveTransaction(invalid_transaction), RegtestNodeError::kMempoolFailure);

    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(RegtestHarness, RejectsFarFutureBlocksWithoutChangingLocalMiningTime)
{
    const auto root = std::filesystem::temp_directory_path() / "novacoin-regtest-clock-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_FALSE(error);
    auto node = RegtestNode::Create(Config(root, "clock", 18544U, 18543U));
    ASSERT_NE(node, nullptr);
    const auto original_tip = node->tip();
    const auto original_next_time = node->next_mining_time();
    const auto far_future = FarFutureBlock(*node);
    ASSERT_TRUE(far_future.has_value());

    EXPECT_EQ(node->ReceiveBlock(*far_future), RegtestNodeError::kBlockRejected);
    EXPECT_EQ(node->tip(), original_tip);
    EXPECT_EQ(node->next_mining_time(), original_next_time);
    EXPECT_EQ(node->MineBlock().error, RegtestNodeError::kNone);
    EXPECT_EQ(node->height(), 1U);
    EXPECT_EQ(node->next_mining_time(), original_next_time + 1U);

    node.reset();
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(RegtestHarness, RefusesDisabledTestnetBeforeCreatingPersistentState)
{
    const auto root = std::filesystem::temp_directory_path() / "novacoin-disabled-testnet";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_FALSE(error);
    auto config = Config(root, "testnet", 18644U, 18643U);
    config.network = nova::consensus::NetworkId::kTestnet;

    EXPECT_EQ(RegtestNode::Create(config), nullptr);
    EXPECT_FALSE(std::filesystem::exists(config.data_directory));

    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(RegtestHarness, SaturatesOperationalMetricsWithoutAffectingChainState)
{
    const auto root = std::filesystem::temp_directory_path() / "novacoin-metrics-bounds";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_FALSE(error);
    auto node = RegtestNode::Create(Config(root, "metrics", 18744U, 18743U));
    ASSERT_NE(node, nullptr);
    const auto original_tip = node->tip();
    node->RecordTransportResult({nova::net::TransportError::kSocketFailure,
                                 std::numeric_limits<std::size_t>::max(),
                                 std::numeric_limits<std::size_t>::max()});
    node->RecordTransportResult({nova::net::TransportError::kSocketFailure, 1U, 1U});

    const auto metrics = node->metrics();
    EXPECT_EQ(metrics.p2p_messages_dispatched, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(metrics.p2p_disconnects, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(metrics.p2p_transport_errors, 2U);
    EXPECT_EQ(metrics.last_block_arrival_time_seconds, 0U);
    EXPECT_EQ(node->tip(), original_tip);

    node.reset();
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(RegtestHarness, RecordsOnlyLocalBlockArrivalForOperationalTelemetry)
{
    const auto root = std::filesystem::temp_directory_path() / "novacoin-block-arrival-metrics";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_FALSE(error);
    auto node = RegtestNode::Create(Config(root, "arrival", 18844U, 18843U));
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->metrics().last_block_arrival_time_seconds, 0U);

    ASSERT_EQ(node->MineBlock().error, RegtestNodeError::kNone);
    EXPECT_GT(node->metrics().last_block_arrival_time_seconds, 0U);

    node.reset();
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

} // namespace
