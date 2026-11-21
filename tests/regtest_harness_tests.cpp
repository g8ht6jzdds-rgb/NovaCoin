#include <gtest/gtest.h>

#include "daemon/durable_migration.hpp"
#include "daemon/durable_runtime.hpp"
#include "daemon/durable_startup.hpp"
#include "node/durable_node.hpp"
#include "node/durable_session.hpp"
#include "node/generation_migration.hpp"
#include "node/generation_recovery.hpp"
#include "node/mining_helpers.hpp"
#include "node/peer_service.hpp"
#include "node/prepared_block.hpp"
#include "node/regtest_node.hpp"
#include "node/staging_workspace.hpp"
#include "rpc/durable_bindings.hpp"

#include "consensus/monetary.hpp"
#include "consensus/network_params.hpp"
#include "consensus/pow.hpp"
#include "primitives/block.hpp"
#include "wallet/address.hpp"
#include "wallet/secure_memory.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace nova::node
{
class PeerServiceTestAccess final
{
  public:
    static void Handle(PeerService& service, const net::FramedMessage& message)
    {
        service.Handle(1U, message);
    }
    static bool Failed(const PeerService& service) noexcept
    {
        return service.durable_failed_;
    }
};
} // namespace nova::node

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

TEST(DurableStagingWorkspace, CleansOnlyIssuedKnownFilesAndPreservesPriorLeftovers)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-staging-workspace-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    EXPECT_EQ(nova::node::StagingWorkspace::Open(root / "missing"), nullptr);
    ASSERT_TRUE(std::filesystem::create_directory(root / "stage-0"));
    auto workspace = nova::node::StagingWorkspace::Open(root);
    ASSERT_NE(workspace, nullptr);
    const auto first = workspace->Next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->filename(), "stage-1");
    EXPECT_FALSE(workspace->Next().has_value());
    EXPECT_FALSE(workspace->Cleanup(root / "stage-0"));
    EXPECT_FALSE(workspace->Cleanup(*first / ".."));
    ASSERT_TRUE(std::filesystem::create_directory(*first));
    for (const auto* name : {"blocks.dat", "wallet.dat", "wallet.dat.tmp", "mempool.dat"}) {
        std::ofstream output{*first / name, std::ios::binary};
        output << "test";
        ASSERT_TRUE(output.good());
    }
    {
        std::ofstream output{*first / "unknown", std::ios::binary};
        output << "preserve";
        ASSERT_TRUE(output.good());
    }
    EXPECT_FALSE(workspace->Cleanup(*first));
    EXPECT_TRUE(std::filesystem::exists(*first / "wallet.dat"));
    EXPECT_TRUE(std::filesystem::exists(*first / "unknown"));
    ASSERT_TRUE(std::filesystem::remove(*first / "unknown"));
    EXPECT_TRUE(workspace->Cleanup(*first));
    EXPECT_FALSE(std::filesystem::exists(*first));
    EXPECT_TRUE(std::filesystem::exists(root / "stage-0"));
    const auto second = workspace->Next();
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(workspace->Cleanup(*second)); // operation failed before directory creation
    workspace.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurableStartup, IdentityAdmissionAllowsOnlyTheHeldLeaseDirectory)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-identity-lease-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    const auto& network = nova::consensus::RegtestNetworkParams();
    auto lease = nova::node::AcquireDataDirectoryLease(root, network);
    ASSERT_NE(lease, nullptr);
    EXPECT_EQ(nova::storage::EnsureNetworkIdentity(root, network),
              nova::storage::NetworkIdentityError::kLegacyDirectoryWithoutMarker);
    {
        std::ofstream file{root / "unknown"};
        file << "preserve";
    }
    EXPECT_EQ(nova::storage::EnsureNetworkIdentityWithLease(root, network),
              nova::storage::NetworkIdentityError::kLegacyDirectoryWithoutMarker);
    EXPECT_FALSE(std::filesystem::exists(root / "network.identity"));
    ASSERT_TRUE(std::filesystem::remove(root / "unknown"));
    EXPECT_EQ(nova::storage::EnsureNetworkIdentityWithLease(root, network),
              nova::storage::NetworkIdentityError::kNone);
    EXPECT_EQ(nova::storage::EnsureNetworkIdentity(root, network),
              nova::storage::NetworkIdentityError::kNone);
    lease.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurableStartup, InitializesOnceRecoversAndRefusesLegacyAndIncompleteStores)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-durable-startup-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const std::vector<std::uint8_t> passphrase{'t', 'e', 's', 't'};
    const nova::rpc::RpcConfig config{
        "127.0.0.1", "user", "pass", {256U, 8192U, 4096U}, nova::consensus::NetworkId::kRegtest};
    auto runtime =
        nova::daemon::OpenDurableRegtest(root / "fresh", time, kStartTime, passphrase, config);
    ASSERT_NE(runtime, nullptr);
    const auto tip = runtime->View()->tip;
    auto legacy_config = Config(root, "legacy-reader", 18444U, 18443U);
    legacy_config.data_directory = root / "fresh";
    EXPECT_EQ(RegtestNode::Create(legacy_config), nullptr);
    EXPECT_FALSE(std::filesystem::exists(root / "fresh" / "wallet.dat"));
    EXPECT_FALSE(std::filesystem::exists(root / "fresh" / "blocks.dat"));
    const auto result = runtime->Rpc().HandleHttpPost(
        {"POST", "Basic dXNlcjpwYXNz", R"({"jsonrpc":"2.0","id":1,"method":"getnewaddress"})"});
    EXPECT_EQ(result.body.find("\"error\""), std::string::npos);
    runtime.reset();
    runtime =
        nova::daemon::OpenDurableRegtest(root / "fresh", time, kStartTime, passphrase, config);
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->View()->tip, tip);
    EXPECT_EQ(runtime->View()->receiving_address_count, 1U);
    EXPECT_EQ(
        nova::daemon::OpenDurableRegtest(root / "fresh", time, kStartTime, passphrase, config),
        nullptr);
    runtime.reset();
    EXPECT_EQ(RegtestNode::Create(legacy_config), nullptr);
    EXPECT_FALSE(std::filesystem::exists(root / "fresh" / "wallet.dat"));
    EXPECT_FALSE(std::filesystem::exists(root / "fresh" / "blocks.dat"));
    ASSERT_EQ(nova::storage::EnsureNetworkIdentity(root / "legacy",
                                                   nova::consensus::RegtestNetworkParams()),
              nova::storage::NetworkIdentityError::kNone);
    {
        std::ofstream file{root / "legacy" / "wallet.dat"};
        file << "legacy";
    }
    EXPECT_EQ(
        nova::daemon::OpenDurableRegtest(root / "legacy", time, kStartTime, passphrase, config),
        nullptr);
    EXPECT_FALSE(std::filesystem::exists(root / "legacy" / "generations"));
    EXPECT_EQ(std::filesystem::file_size(root / "legacy" / "wallet.dat"), 6U);
    auto active_legacy_config = Config(root, "active-legacy", 18444U, 18443U);
    auto active_legacy = RegtestNode::Create(active_legacy_config);
    ASSERT_NE(active_legacy, nullptr);
    EXPECT_EQ(RegtestNode::Create(active_legacy_config), nullptr);
    EXPECT_EQ(nova::daemon::OpenDurableRegtest(active_legacy_config.data_directory, time,
                                               kStartTime, passphrase, config),
              nullptr);
    EXPECT_FALSE(std::filesystem::exists(active_legacy_config.data_directory / "generations"));
    active_legacy.reset();
    active_legacy = RegtestNode::Create(active_legacy_config);
    ASSERT_NE(active_legacy, nullptr);
    active_legacy.reset();
    ASSERT_EQ(nova::storage::EnsureNetworkIdentity(root / "incomplete",
                                                   nova::consensus::RegtestNetworkParams()),
              nova::storage::NetworkIdentityError::kNone);
    ASSERT_TRUE(std::filesystem::create_directory(root / "incomplete" / "generations"));
    EXPECT_EQ(
        nova::daemon::OpenDurableRegtest(root / "incomplete", time, kStartTime, passphrase, config),
        nullptr);
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurableStartup, MigratesOfflineLegacyToNewDestinationWithoutChangingSource)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-startup-migrate-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    const auto legacy_config = Config(root, "source", 18444U, 18443U);
    auto legacy = RegtestNode::Create(legacy_config);
    ASSERT_NE(legacy, nullptr);
    ASSERT_EQ(legacy->MineBlock().error, RegtestNodeError::kNone);
    const auto tip = legacy->tip();
    const auto balance = legacy->wallet().ConfirmedBalance();
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const nova::rpc::RpcConfig config{
        "127.0.0.1", "user", "pass", {256U, 8192U, 4096U}, nova::consensus::NetworkId::kRegtest};
    EXPECT_EQ(nova::daemon::MigrateDurableRegtest(legacy_config.data_directory, root / "busy", time,
                                                  kStartTime, legacy_config.wallet_passphrase,
                                                  config),
              nullptr);
    EXPECT_FALSE(std::filesystem::exists(root / "busy"));
    auto read = [](const auto& path) {
        std::ifstream file{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    };
    const auto journal = read(legacy_config.data_directory / "blocks.dat");
    const auto wallet = read(legacy_config.data_directory / "wallet.dat");
    legacy.reset();
    EXPECT_EQ(nova::daemon::MigrateDurableRegtest(
                  legacy_config.data_directory, legacy_config.data_directory / "nested", time,
                  kStartTime, legacy_config.wallet_passphrase, config),
              nullptr);
    const std::vector<std::uint8_t> wrong{'w', 'r', 'o', 'n', 'g'};
    EXPECT_EQ(nova::daemon::MigrateDurableRegtest(legacy_config.data_directory, root / "wrong",
                                                  time, kStartTime, wrong, config),
              nullptr);
    auto runtime =
        nova::daemon::MigrateDurableRegtest(legacy_config.data_directory, root / "target", time,
                                            kStartTime, legacy_config.wallet_passphrase, config);
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(runtime->View().has_value());
    EXPECT_EQ(runtime->View()->height, 1U);
    EXPECT_EQ(runtime->View()->tip, tip);
    EXPECT_EQ(runtime->View()->confirmed_balance, balance);
    EXPECT_EQ(runtime->View()->mempool_size, 0U);
    runtime.reset();
    runtime = nova::daemon::OpenDurableRegtest(root / "target", time, kStartTime,
                                               legacy_config.wallet_passphrase, config);
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->View()->tip, tip);
    runtime.reset();
    EXPECT_EQ(read(legacy_config.data_directory / "blocks.dat"), journal);
    EXPECT_EQ(read(legacy_config.data_directory / "wallet.dat"), wallet);
    EXPECT_EQ(nova::daemon::MigrateDurableRegtest(legacy_config.data_directory, root / "target",
                                                  time, kStartTime, legacy_config.wallet_passphrase,
                                                  config),
              nullptr);
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(DurableTestnetMigration, RejectsUnsafeSourcesAndPreservesFrozenIdentityOnRestart)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-testnet-migration-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    auto source_config = Config(root, "source", 29161U, 29162U);
    source_config.network = nova::consensus::NetworkId::kTestnet;
    source_config.start_time =
        nova::consensus::TestnetNetworkParams().genesis_block.header.time + 600U;
    auto source = RegtestNode::Create(source_config);
    ASSERT_NE(source, nullptr);
    ASSERT_TRUE(source->GenerateReceivingAddress());
    const auto tip = source->tip();
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const nova::rpc::RpcConfig config{
        "127.0.0.1", "user", "pass", {256U, 8192U, 4096U}, nova::consensus::NetworkId::kTestnet};
    const auto migrate = [&](const char* name) {
        return nova::daemon::MigrateDurableTestnet(source_config.data_directory, root / name, time,
                                                   source_config.start_time,
                                                   source_config.wallet_passphrase, config);
    };
    EXPECT_EQ(migrate("busy"), nullptr);
    EXPECT_FALSE(std::filesystem::exists(root / "busy"));
    source.reset();
    EXPECT_EQ(nova::daemon::MigrateDurableTestnet(
                  source_config.data_directory, source_config.data_directory / "nested", time,
                  source_config.start_time, source_config.wallet_passphrase, config),
              nullptr);
    EXPECT_FALSE(std::filesystem::exists(source_config.data_directory / "nested"));
    const auto read = [](const auto& path) {
        std::ifstream input{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    };
    const auto identity_path = source_config.data_directory / "network.identity";
    const auto original_identity = read(identity_path);
    const auto original_wallet = read(source_config.data_directory / "wallet.dat");
    const auto original_journal = read(source_config.data_directory / "blocks.dat");
    ASSERT_TRUE(std::filesystem::create_directory(source_config.data_directory / "generations"));
    EXPECT_EQ(migrate("mixed"), nullptr);
    EXPECT_FALSE(std::filesystem::exists(root / "mixed"));
    ASSERT_TRUE(std::filesystem::remove(source_config.data_directory / "generations"));
    const auto retained_identity = source_config.data_directory / "identity-retained";
    std::filesystem::rename(identity_path, retained_identity);
    EXPECT_EQ(migrate("unidentified"), nullptr);
    EXPECT_FALSE(std::filesystem::exists(identity_path));
    EXPECT_FALSE(std::filesystem::exists(root / "unidentified"));
    std::filesystem::rename(retained_identity, identity_path);
    auto wrong_network = config;
    wrong_network.network = nova::consensus::NetworkId::kRegtest;
    EXPECT_EQ(nova::daemon::MigrateDurableRegtest(
                  source_config.data_directory, root / "wrong-network", time,
                  source_config.start_time, source_config.wallet_passphrase, wrong_network),
              nullptr);
    EXPECT_FALSE(std::filesystem::exists(root / "wrong-network"));
    auto migrated = migrate("accepted");
    ASSERT_NE(migrated, nullptr);
    ASSERT_TRUE(migrated->View());
    EXPECT_EQ(migrated->View()->tip, tip);
    EXPECT_EQ(migrated->View()->height, 0U);
    EXPECT_EQ(migrated->View()->receiving_address_count, 1U);
    migrated.reset();
    migrated = nova::daemon::OpenDurableNode(root / "accepted", time, source_config.start_time,
                                             source_config.wallet_passphrase, config);
    ASSERT_NE(migrated, nullptr);
    ASSERT_TRUE(migrated->View());
    EXPECT_EQ(migrated->View()->tip, tip);
    EXPECT_EQ(migrated->View()->receiving_address_count, 1U);
    EXPECT_EQ(read(identity_path), original_identity);
    EXPECT_EQ(read(source_config.data_directory / "wallet.dat"), original_wallet);
    EXPECT_EQ(read(source_config.data_directory / "blocks.dat"), original_journal);
    migrated.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurableNode, DirectBlockIngressRejectsWithoutPublicationAndRecoversRetry)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-node-owner-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    auto producer = RegtestNode::Create(Config(root, "producer", 29141U, 29142U));
    ASSERT_NE(producer, nullptr);
    const auto mined = producer->MineBlock();
    ASSERT_TRUE(mined.block_hash);
    const auto index = producer->chain_state().GetBlockIndex(*mined.block_hash);
    ASSERT_TRUE(index && index->block);
    const auto block = *index->block;
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const std::vector<std::uint8_t> password{'t', 'e', 's', 't'};
    const auto network = nova::consensus::NetworkId::kRegtest;
    auto opened =
        nova::node::OpenDurableNodeState(root / "receiver", network, time, kStartTime, password);
    ASSERT_NE(opened, nullptr);
    auto* fault_session = opened->session.get();
    auto owner =
        nova::node::DurableNode::Create(std::move(opened->session), std::move(opened->workspace),
                                        password, std::move(opened->directory_lease));
    ASSERT_NE(owner, nullptr);
    const auto before = owner->View();
    ASSERT_TRUE(before);
    auto invalid = block;
    invalid.header.merkle_root = nova::crypto::Hash256{};
    EXPECT_NE(owner->ReceiveBlock(invalid).error, nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(owner->View());
    EXPECT_EQ(owner->View()->tip, before->tip);
    const auto read_selector = [&] {
        std::ifstream input{root / "receiver" / "generations" / "generation.current",
                            std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    };
    const auto selector_before = read_selector();
    ASSERT_FALSE(selector_before.empty());
    const auto unchanged = [&] {
        const auto current = owner->View();
        ASSERT_TRUE(current);
        EXPECT_EQ(current->tip, before->tip);
        EXPECT_EQ(current->height, before->height);
        EXPECT_EQ(current->chain_work, before->chain_work);
        EXPECT_EQ(current->confirmed_balance, before->confirmed_balance);
        EXPECT_EQ(current->mempool_size, before->mempool_size);
        EXPECT_EQ(current->receiving_address_count, before->receiving_address_count);
        EXPECT_EQ(read_selector(), selector_before);
        const auto committed = owner->Session().CommittedBlocks();
        ASSERT_TRUE(committed);
        EXPECT_TRUE(committed->empty());
    };
    using Target = nova::storage::GenerationIoTarget;
    using Fault = nova::storage::GenerationIoFault;
    for (const auto target : {Target::kJournal, Target::kWallet, Target::kMempool,
                              Target::kDescriptor, Target::kRoot}) {
        for (const auto fault : {Fault::kZeroProgress, Fault::kNoSpace, Fault::kPrefixThenNoSpace,
                                 Fault::kSyncFailure}) {
            SCOPED_TRACE(static_cast<unsigned>(target));
            SCOPED_TRACE(static_cast<unsigned>(fault));
            fault_session->SetStoreIoFaultForTesting(target, fault);
            EXPECT_EQ(owner->ReceiveBlock(block).error, nova::node::DurableSessionError::kStorage);
            EXPECT_TRUE(fault_session->StoreIoFaultTriggeredForTesting());
            unchanged();
            for (unsigned restart = 0U; restart < 2U; ++restart) {
                ASSERT_TRUE(owner->Shutdown());
                owner.reset();
                opened = nova::node::OpenDurableNodeState(root / "receiver", network, time,
                                                          kStartTime, password);
                ASSERT_NE(opened, nullptr);
                fault_session = opened->session.get();
                owner = nova::node::DurableNode::Create(std::move(opened->session),
                                                        std::move(opened->workspace), password,
                                                        std::move(opened->directory_lease));
                ASSERT_NE(owner, nullptr);
                unchanged();
            }
        }
    }
    fault_session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterWallet);
    EXPECT_EQ(owner->ReceiveBlock(block).error, nova::node::DurableSessionError::kStorage);
    ASSERT_TRUE(owner->View());
    EXPECT_EQ(owner->View()->tip, before->tip);
    EXPECT_EQ(owner->View()->height, before->height);
    EXPECT_EQ(owner->View()->confirmed_balance, before->confirmed_balance);
    EXPECT_EQ(owner->View()->mempool_size, before->mempool_size);
    EXPECT_TRUE(std::filesystem::is_empty(root / "receiver" / "staging"));
    owner.reset();
    opened =
        nova::node::OpenDurableNodeState(root / "receiver", network, time, kStartTime, password);
    ASSERT_NE(opened, nullptr);
    owner =
        nova::node::DurableNode::Create(std::move(opened->session), std::move(opened->workspace),
                                        password, std::move(opened->directory_lease));
    ASSERT_NE(owner, nullptr);
    ASSERT_TRUE(owner->View());
    EXPECT_EQ(owner->View()->tip, before->tip);
    EXPECT_EQ(owner->View()->height, before->height);
    const auto empty_history = owner->Session().CommittedBlocks();
    ASSERT_TRUE(empty_history.has_value());
    EXPECT_TRUE(empty_history->empty());
    EXPECT_EQ(owner->ReceiveBlock(block).error, nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(owner->View());
    EXPECT_EQ(owner->View()->tip, *mined.block_hash);
    EXPECT_EQ(owner->View()->height, 1U);
    const auto history = owner->Session().CommittedBlocks();
    ASSERT_TRUE(history.has_value());
    ASSERT_EQ(history->size(), 1U);
    EXPECT_EQ(nova::primitives::ComputeBlockHash(history->front().header), mined.block_hash);
    owner.reset();
    opened =
        nova::node::OpenDurableNodeState(root / "receiver", network, time, kStartTime, password);
    ASSERT_NE(opened, nullptr);
    ASSERT_TRUE(opened->session->View());
    EXPECT_EQ(opened->session->View()->tip, *mined.block_hash);
    EXPECT_EQ(opened->session->View()->height, 1U);
    opened->session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_FALSE(opened->session->CommittedBlocks().has_value());
    const auto recovered_history = opened->session->CommittedBlocks();
    ASSERT_TRUE(recovered_history.has_value());
    ASSERT_EQ(recovered_history->size(), 1U);
    EXPECT_EQ(nova::primitives::ComputeBlockHash(recovered_history->front().header),
              mined.block_hash);
    auto fork_producer = RegtestNode::Create(Config(root, "history-fork", 29311U, 29312U));
    ASSERT_NE(fork_producer, nullptr);
    const auto fork_mined = fork_producer->MineBlock();
    ASSERT_EQ(fork_mined.error, RegtestNodeError::kNone);
    ASSERT_TRUE(fork_mined.block_hash.has_value());
    ASSERT_NE(fork_mined.block_hash, mined.block_hash);
    const auto fork_index = fork_producer->chain_state().GetBlockIndex(*fork_mined.block_hash);
    ASSERT_TRUE(fork_index.has_value());
    ASSERT_TRUE(fork_index->block.has_value());
    ASSERT_EQ(
        opened->session->ApplyBlock(*fork_index->block, {}, root / "history-stage", password).error,
        nova::node::DurableSessionError::kNone);
    const auto branches = opened->session->CommittedBlocks();
    ASSERT_TRUE(branches.has_value());
    ASSERT_EQ(branches->size(), 2U);
    EXPECT_EQ(nova::primitives::ComputeBlockHash(branches->at(0U).header), mined.block_hash);
    EXPECT_EQ(nova::primitives::ComputeBlockHash(branches->at(1U).header), fork_mined.block_hash);
    fork_producer.reset();
    opened.reset();
    opened =
        nova::node::OpenDurableNodeState(root / "receiver", network, time, kStartTime, password);
    ASSERT_NE(opened, nullptr);
    const auto restarted_branches = opened->session->CommittedBlocks();
    ASSERT_TRUE(restarted_branches.has_value());
    ASSERT_EQ(restarted_branches->size(), 2U);
    EXPECT_EQ(nova::primitives::ComputeBlockHash(restarted_branches->at(0U).header),
              mined.block_hash);
    EXPECT_EQ(nova::primitives::ComputeBlockHash(restarted_branches->at(1U).header),
              fork_mined.block_hash);
    opened.reset();
    producer.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurableRuntime, RpcWritesShareDurableOwnerAndRecoverOnRestart)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-durable-runtime-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    ASSERT_TRUE(std::filesystem::create_directory(root / "store"));
    ASSERT_TRUE(std::filesystem::create_directory(root / "staging"));
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const std::vector<std::uint8_t> passphrase{'t', 'e', 's', 't'};
    nova::storage::GenerationError error{};
    auto store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    auto session = nova::node::DurableSession::CreateFresh(
        std::move(store), nova::consensus::NetworkId::kRegtest, time, kStartTime, passphrase,
        root / "initial");
    ASSERT_NE(session, nullptr);
    const auto receiving = session->GenerateAddress(root / "first-address", passphrase);
    ASSERT_EQ(receiving.error, nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(receiving.address.has_value());
    const auto encoded =
        nova::wallet::EncodeP2pkhAddress(receiving.address->public_key_hash, network);
    ASSERT_TRUE(encoded.has_value());
    auto workspace = nova::node::StagingWorkspace::Open(root / "staging");
    ASSERT_NE(workspace, nullptr);
    auto* const workspace_observer = workspace.get();
    auto* const session_observer = session.get();
    nova::rpc::RpcConfig config{
        "127.0.0.1", "user", "pass", {256U, 8192U, 4096U}, nova::consensus::NetworkId::kRegtest};
    auto runtime = nova::daemon::DurableRuntime::Create(std::move(session), std::move(workspace),
                                                        passphrase, config);
    ASSERT_NE(runtime, nullptr);
    constexpr std::string_view auth{"Basic dXNlcjpwYXNz"};
    const auto address = runtime->Rpc().HandleHttpPost(
        {"POST", auth, R"({"jsonrpc":"2.0","id":1,"method":"getnewaddress"})"});
    ASSERT_EQ(address.body.find("\"error\""), std::string::npos);
    EXPECT_EQ(runtime->View()->receiving_address_count, 2U);
    const auto before_failure = runtime->View();
    ASSERT_TRUE(before_failure.has_value());
    session_observer->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterWallet);
    const auto rejected = runtime->Rpc().HandleHttpPost(
        {"POST", auth, R"({"jsonrpc":"2.0","id":20,"method":"generateregtestblock"})"});
    EXPECT_NE(rejected.body.find("\"error\""), std::string::npos);
    ASSERT_TRUE(runtime->View().has_value());
    EXPECT_EQ(runtime->View()->tip, before_failure->tip);
    EXPECT_EQ(runtime->View()->height, before_failure->height);
    EXPECT_EQ(runtime->View()->receiving_address_count, before_failure->receiving_address_count);
    EXPECT_TRUE(std::filesystem::is_empty(root / "staging"));
    const auto mined = runtime->Rpc().HandleHttpPost(
        {"POST", auth, R"({"jsonrpc":"2.0","id":2,"method":"generateregtestblock"})"});
    ASSERT_EQ(mined.body.find("\"error\""), std::string::npos);
    ASSERT_TRUE(runtime->View().has_value());
    EXPECT_EQ(runtime->View()->height, 1U);
    const auto tip = runtime->View()->tip;
    const auto send = std::string{"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"sendtoaddress\","
                                  "\"params\":{\"address\":\""} +
                      *encoded + "\",\"amount\":1}}";
    const auto payment = runtime->Rpc().HandleHttpPost({"POST", auth, send});
    ASSERT_EQ(payment.body.find("\"error\""), std::string::npos);
    ASSERT_TRUE(runtime->View().has_value());
    EXPECT_EQ(runtime->View()->mempool_size, 1U);
    EXPECT_TRUE(std::filesystem::is_empty(root / "staging"));
    const auto reserved = workspace_observer->Next();
    ASSERT_TRUE(reserved.has_value());
    const auto blocked_address = runtime->Rpc().HandleHttpPost(
        {"POST", auth, R"({"jsonrpc":"2.0","id":4,"method":"getnewaddress"})"});
    EXPECT_NE(blocked_address.body.find("operation outcome may have committed"), std::string::npos);
    EXPECT_FALSE(runtime->Ready());
    EXPECT_EQ(runtime->Peer().Pump(0U).error, nova::net::TransportError::kSocketFailure);
    const auto unavailable = runtime->Rpc().HandleHttpPost(
        {"POST", auth, R"({"jsonrpc":"2.0","id":5,"method":"getblockchaininfo"})"});
    EXPECT_NE(unavailable.body.find("node state unavailable"), std::string::npos);
    EXPECT_TRUE(workspace_observer->Cleanup(*reserved));
    runtime.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(
        std::move(store), nova::consensus::NetworkId::kRegtest, time, kStartTime, passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->tip, tip);
    EXPECT_EQ(session->View()->height, 1U);
    EXPECT_EQ(session->View()->mempool_size, 1U);
    EXPECT_EQ(session->View()->receiving_address_count, 2U);
    workspace = nova::node::StagingWorkspace::Open(root / "staging");
    runtime = nova::daemon::DurableRuntime::Create(std::move(session), std::move(workspace),
                                                   passphrase, config);
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(runtime->Ready());
    runtime->Peer().QuarantineDurable();
    EXPECT_FALSE(runtime->Ready());
    EXPECT_EQ(runtime->Peer().Pump(0U).error, nova::net::TransportError::kSocketFailure);
    const auto peer_unavailable = runtime->Rpc().HandleHttpPost(
        {"POST", auth, R"({"jsonrpc":"2.0","id":6,"method":"getnewaddress"})"});
    EXPECT_NE(peer_unavailable.body.find("node state unavailable"), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_empty(root / "staging"));
    runtime.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurableRuntime, UncertainPaymentReturnsCandidateIdForRecovery)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-uncertain-payment-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_TRUE(std::filesystem::create_directory(root / "store"));
    ASSERT_TRUE(std::filesystem::create_directory(root / "staging"));
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const std::vector<std::uint8_t> passphrase{'t', 'e', 's', 't'};
    nova::storage::GenerationError error{};
    auto store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    auto session = nova::node::DurableSession::CreateFresh(
        std::move(store), network.id, time, kStartTime, passphrase, root / "initial");
    ASSERT_NE(session, nullptr);
    const auto receiving = session->GenerateAddress(root / "address", passphrase);
    ASSERT_TRUE(receiving.address);
    const auto encoded =
        nova::wallet::EncodeP2pkhAddress(receiving.address->public_key_hash, network);
    ASSERT_TRUE(encoded);
    auto* const session_observer = session.get();
    auto workspace = nova::node::StagingWorkspace::Open(root / "staging");
    const nova::rpc::RpcConfig config{
        "127.0.0.1", "user", "pass", {256U, 8192U, 4096U}, nova::consensus::NetworkId::kRegtest};
    auto runtime = nova::daemon::DurableRuntime::Create(std::move(session), std::move(workspace),
                                                        passphrase, config);
    ASSERT_NE(runtime, nullptr);
    constexpr std::string_view auth{"Basic dXNlcjpwYXNz"};
    const auto mined = runtime->Rpc().HandleHttpPost(
        {"POST", auth, R"({"jsonrpc":"2.0","id":1,"method":"generateregtestblock"})"});
    ASSERT_EQ(mined.body.find("\"error\""), std::string::npos);
    session_observer->FailAdvanceForTesting();
    const auto send = std::string{"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"sendtoaddress\","
                                  "\"params\":{\"address\":\""} +
                      *encoded + "\",\"amount\":1}}";
    const auto failed = runtime->Rpc().HandleHttpPost({"POST", auth, send});
    constexpr std::string_view label{"candidate transaction id "};
    const auto start = failed.body.find(label);
    ASSERT_NE(start, std::string::npos);
    const auto id = failed.body.substr(start + label.size(), 64U);
    ASSERT_EQ(id.size(), 64U);
    EXPECT_FALSE(runtime->Ready());
    runtime.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(std::move(store), network.id, time, kStartTime,
                                               passphrase);
    ASSERT_NE(session, nullptr);
    workspace = nova::node::StagingWorkspace::Open(root / "staging");
    runtime = nova::daemon::DurableRuntime::Create(std::move(session), std::move(workspace),
                                                   passphrase, config);
    ASSERT_NE(runtime, nullptr);
    const auto query = std::string{"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"getmempoolentry\","
                                   "\"params\":{\"txid\":\""} +
                       id + "\"}}";
    const auto found = runtime->Rpc().HandleHttpPost({"POST", auth, query});
    EXPECT_EQ(found.body.find("\"error\""), std::string::npos);
    EXPECT_EQ(runtime->View()->mempool_size, 1U);
    runtime.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurablePeer, HigherPriorityInvalidCompetingBranchDoesNotPublish)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-invalid-competing-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_TRUE(std::filesystem::create_directory(root / "store"));
    auto producer = RegtestNode::Create(Config(root, "producer", 29101U, 29102U));
    ASSERT_NE(producer, nullptr);
    std::vector<nova::primitives::Block> main_blocks;
    for (unsigned i = 0; i < 4; ++i) {
        const auto mined = producer->MineBlock();
        ASSERT_TRUE(mined.block_hash);
        const auto index = producer->chain_state().GetBlockIndex(*mined.block_hash);
        ASSERT_TRUE(index && index->block);
        main_blocks.push_back(*index->block);
    }
    // Keep the tie discriminator away from the minimum hash so fixture
    // search does not depend on a randomly generated wallet key.
    const auto original_time = main_blocks.back().header.time;
    bool high_active_hash = false;
    for (std::uint32_t delta = 0; delta < 1024U; ++delta) {
        auto header = main_blocks.back().header;
        header.time = original_time + delta;
        const auto proof = nova::consensus::MineRegtestBlock(header, 4096U);
        ASSERT_TRUE(proof);
        if (proof->block_hash.bytes().front() >= 128U) {
            main_blocks.back().header = proof->header;
            high_active_hash = true;
            break;
        }
    }
    ASSERT_TRUE(high_active_hash);
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const std::vector<std::uint8_t> passphrase{'t', 'e', 's', 't'};
    nova::storage::GenerationError error{};
    auto store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    auto* store_observer = store.get();
    auto session = nova::node::DurableSession::CreateFresh(
        std::move(store), network.id, time, kStartTime, passphrase, root / "initial");
    ASSERT_NE(session, nullptr);
    for (std::size_t i = 0; i < main_blocks.size(); ++i)
        ASSERT_EQ(
            session
                ->ApplyBlock(main_blocks[i], {}, root / ("main-" + std::to_string(i)), passphrase)
                .error,
            nova::node::DurableSessionError::kNone);
    const auto active_tip = session->View()->tip;
    const auto active_coinbase =
        main_blocks.back().transactions.front().TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(active_coinbase);
    const nova::chain::UTXOKey active_key{*active_coinbase, 0U};
    std::vector<nova::chain::UTXOKey> side_keys;
    auto side_parent = *nova::primitives::ComputeBlockHash(main_blocks[0].header);
    for (std::size_t i = 1; i < 3; ++i) {
        auto side = main_blocks[i];
        side.header.previous_block_id = side_parent;
        side.transactions.front().outputs.front().script_pubkey[3] ^= 1U;
        const auto merkle = nova::primitives::ComputeMerkleRoot(
            side.transactions, network.block_limits.transaction_limits);
        ASSERT_TRUE(merkle);
        side.header.merkle_root = *merkle;
        const auto side_coinbase =
            side.transactions.front().TxId(network.block_limits.transaction_limits);
        ASSERT_TRUE(side_coinbase);
        side_keys.push_back({*side_coinbase, 0U});
        const auto proof = nova::consensus::MineRegtestBlock(side.header, 4096U);
        ASSERT_TRUE(proof);
        side.header = proof->header;
        side_parent = proof->block_hash;
        const auto accepted =
            session->ApplyBlock(side, {}, root / ("side-" + std::to_string(i)), passphrase);
        ASSERT_EQ(accepted.error, nova::node::DurableSessionError::kNone);
        EXPECT_FALSE(accepted.chain_result.became_active);
    }
    auto invalid = main_blocks[3];
    invalid.header.previous_block_id = side_parent;
    ++invalid.transactions.front().outputs.front().value;
    const auto merkle = nova::primitives::ComputeMerkleRoot(
        invalid.transactions, network.block_limits.transaction_limits);
    ASSERT_TRUE(merkle);
    invalid.header.merkle_root = *merkle;
    bool wins_tie = false;
    for (std::uint32_t delta = 0; delta < 1024U; ++delta) {
        invalid.header.time = main_blocks[3].header.time + delta;
        const auto proof = nova::consensus::MineRegtestBlock(invalid.header, 4096U);
        ASSERT_TRUE(proof);
        if (proof->block_hash.bytes() < active_tip.bytes()) {
            invalid.header = proof->header;
            wins_tie = true;
            break;
        }
    }
    ASSERT_TRUE(wins_tie);
    const auto invalid_hash = nova::primitives::ComputeBlockHash(invalid.header);
    ASSERT_TRUE(invalid_hash);
    const auto before = session->View();
    ASSERT_TRUE(before);
    const auto generation_before = store_observer->Observe();
    ASSERT_EQ(generation_before.state, nova::storage::GenerationState::kPresent);
    const auto read_root = [&] {
        std::ifstream input{root / "store" / "generation.current", std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    };
    const auto root_before = read_root();
    ASSERT_FALSE(root_before.empty());
    const auto check_generation = [&] {
        const auto generation = store_observer->Observe();
        ASSERT_EQ(generation.state, nova::storage::GenerationState::kPresent);
        EXPECT_EQ(generation.snapshot.sequence, generation_before.snapshot.sequence);
        EXPECT_EQ(generation.snapshot.journal_hash, generation_before.snapshot.journal_hash);
        EXPECT_EQ(generation.snapshot.wallet_hash, generation_before.snapshot.wallet_hash);
        EXPECT_EQ(generation.snapshot.mempool_hash, generation_before.snapshot.mempool_hash);
        EXPECT_EQ(generation.snapshot.journal_size, generation_before.snapshot.journal_size);
        EXPECT_EQ(generation.snapshot.wallet_size, generation_before.snapshot.wallet_size);
        EXPECT_EQ(generation.snapshot.mempool_size, generation_before.snapshot.mempool_size);
        EXPECT_EQ(read_root(), root_before);
    };
    const auto check_coins = [&] {
        const auto active = session->ReadCoin(active_key);
        EXPECT_EQ(active.error, nova::chain::CoinReadError::kNone);
        EXPECT_TRUE(active.coin);
        for (const auto& key : side_keys) {
            const auto side = session->ReadCoin(key);
            EXPECT_EQ(side.error, nova::chain::CoinReadError::kNone);
            EXPECT_FALSE(side.coin);
        }
    };
    check_coins();
    nova::node::DurableSessionResult observed;
    auto service = nova::node::PeerService::Create(
        *session,
        [&](const nova::primitives::Block& candidate) {
            observed = session->ApplyBlock(candidate, {}, root / "invalid", passphrase);
            return observed;
        },
        [&](const nova::primitives::Transaction&) {
            return nova::node::DurableSessionError::kMempool;
        });
    ASSERT_NE(service, nullptr);
    nova::node::PeerServiceTestAccess::Handle(*service, {nova::net::Command::kBlock, invalid});
    EXPECT_EQ(observed.error, nova::node::DurableSessionError::kChain);
    EXPECT_EQ(observed.chain_result.validation_error,
              nova::chain::BlockValidationError::kInvalidCoinbaseReward);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, before->tip);
    EXPECT_EQ(session->View()->height, before->height);
    EXPECT_EQ(session->View()->chain_work, before->chain_work);
    EXPECT_EQ(session->View()->confirmed_balance, before->confirmed_balance);
    EXPECT_EQ(session->View()->mempool_size, before->mempool_size);
    EXPECT_FALSE(session->FindBlock(*invalid_hash).block);
    check_generation();
    check_coins();
    service.reset();
    session.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    store_observer = store.get();
    session = nova::node::DurableSession::Open(std::move(store), network.id, time, kStartTime,
                                               passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, before->tip);
    EXPECT_EQ(session->View()->chain_work, before->chain_work);
    EXPECT_EQ(session->View()->confirmed_balance, before->confirmed_balance);
    EXPECT_FALSE(session->FindBlock(*invalid_hash).block);
    check_generation();
    check_coins();
    session.reset();
    producer.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurablePeer, InvalidReceivedBlockDoesNotChangeStateOrRestart)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-invalid-branch-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_TRUE(std::filesystem::create_directory(root / "store"));
    auto producer = RegtestNode::Create(Config(root, "producer", 29101U, 29102U));
    ASSERT_NE(producer, nullptr);
    const auto first = producer->MineBlock();
    ASSERT_TRUE(first.block_hash);
    const auto first_index = producer->chain_state().GetBlockIndex(*first.block_hash);
    ASSERT_TRUE(first_index && first_index->block);
    const auto second = producer->MineBlock();
    ASSERT_TRUE(second.block_hash);
    const auto second_index = producer->chain_state().GetBlockIndex(*second.block_hash);
    ASSERT_TRUE(second_index && second_index->block);
    auto invalid = *second_index->block;
    ASSERT_FALSE(invalid.transactions.empty());
    ASSERT_FALSE(invalid.transactions.front().outputs.empty());
    ++invalid.transactions.front().outputs.front().value;
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const std::vector<std::uint8_t> passphrase{'t', 'e', 's', 't'};
    nova::storage::GenerationError error{};
    auto store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    auto session = nova::node::DurableSession::CreateFresh(
        std::move(store), network.id, time, kStartTime, passphrase, root / "initial");
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(session->ApplyBlock(*first_index->block, {}, root / "first", passphrase).error,
              nova::node::DurableSessionError::kNone);
    const auto before = session->View();
    ASSERT_TRUE(before);
    nova::node::DurableSessionResult observed;
    auto service = nova::node::PeerService::Create(
        *session,
        [&](const nova::primitives::Block& candidate) {
            observed = session->ApplyBlock(candidate, {}, root / "invalid", passphrase);
            return observed;
        },
        [&](const nova::primitives::Transaction&) {
            return nova::node::DurableSessionError::kMempool;
        });
    ASSERT_NE(service, nullptr);
    nova::node::PeerServiceTestAccess::Handle(*service, {nova::net::Command::kBlock, invalid});
    EXPECT_EQ(observed.error, nova::node::DurableSessionError::kChain);
    EXPECT_EQ(observed.chain_result.validation_error,
              nova::chain::BlockValidationError::kBadMerkleRoot);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, before->tip);
    EXPECT_EQ(session->View()->height, before->height);
    EXPECT_EQ(session->View()->confirmed_balance, before->confirmed_balance);
    EXPECT_EQ(session->View()->mempool_size, before->mempool_size);
    service.reset();
    session.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(std::move(store), network.id, time, kStartTime,
                                               passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, before->tip);
    EXPECT_EQ(session->View()->height, before->height);
    EXPECT_EQ(session->View()->confirmed_balance, before->confirmed_balance);
    EXPECT_EQ(session->ApplyBlock(*second_index->block, {}, root / "valid", passphrase).error,
              nova::node::DurableSessionError::kNone);
    EXPECT_EQ(session->View()->tip, *second.block_hash);
    session.reset();
    producer.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurablePeer, MissingStagingParentRejectsReceivedBlockAndPreservesRestart)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-durable-staging-io-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_TRUE(std::filesystem::create_directory(root / "store"));
    auto producer = RegtestNode::Create(Config(root, "producer", 29101U, 29102U));
    ASSERT_NE(producer, nullptr);
    const auto mined = producer->MineBlock();
    ASSERT_TRUE(mined.block_hash.has_value());
    const auto index = producer->chain_state().GetBlockIndex(*mined.block_hash);
    ASSERT_TRUE(index && index->block);
    const auto block = *index->block;
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const std::vector<std::uint8_t> passphrase{'t', 'e', 's', 't'};
    nova::storage::GenerationError error{};
    auto store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    auto session = nova::node::DurableSession::CreateFresh(
        std::move(store), network.id, time, kStartTime, passphrase, root / "initial");
    ASSERT_NE(session, nullptr);
    const auto before = session->View();
    ASSERT_TRUE(before);
    nova::node::DurableSessionError failure = nova::node::DurableSessionError::kNone;
    auto service = nova::node::PeerService::Create(
        *session,
        [&](const nova::primitives::Block& candidate) {
            auto result =
                session->ApplyBlock(candidate, {}, root / "missing" / "stage", passphrase);
            failure = result.error;
            return result;
        },
        [&](const nova::primitives::Transaction&) {
            return nova::node::DurableSessionError::kMempool;
        });
    ASSERT_NE(service, nullptr);
    nova::node::PeerServiceTestAccess::Handle(*service, {nova::net::Command::kBlock, block});
    EXPECT_EQ(failure, nova::node::DurableSessionError::kStorage);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, before->tip);
    EXPECT_EQ(session->View()->height, before->height);
    EXPECT_EQ(session->View()->confirmed_balance, before->confirmed_balance);
    EXPECT_FALSE(std::filesystem::exists(root / "missing"));
    service.reset();
    session.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(std::move(store), network.id, time, kStartTime,
                                               passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, before->tip);
    EXPECT_EQ(session->View()->height, before->height);
    const auto retry = session->ApplyBlock(block, {}, root / "retry", passphrase);
    EXPECT_EQ(retry.error, nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, *mined.block_hash);
    session.reset();
    producer.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(DurableTestnetSession, RecoversFrozenGenesisAndWalletWithoutListeners)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-durable-testnet-fixture-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_TRUE(std::filesystem::create_directory(root / "store"));
    const auto& network = nova::consensus::TestnetNetworkParams();
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const std::vector<std::uint8_t> passphrase{'t', 'e', 's', 't'};
    const auto start_time = network.genesis_block.header.time + 600U;
    ASSERT_FALSE(network.genesis_block.transactions.empty());
    const auto& genesis_tx = network.genesis_block.transactions.front();
    ASSERT_FALSE(genesis_tx.outputs.empty());
    const auto id = genesis_tx.TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(id);
    const nova::chain::UTXOKey key{*id, 0U};
    nova::storage::GenerationError error{};
    auto store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    auto session = nova::node::DurableSession::CreateFresh(
        std::move(store), network.id, time, start_time, passphrase, root / "initial");
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, network.genesis_hash);
    EXPECT_EQ(session->View()->height, 0U);
    const auto coin = session->ReadCoin(key);
    ASSERT_EQ(coin.error, nova::chain::CoinReadError::kNone);
    ASSERT_TRUE(coin.coin);
    EXPECT_EQ(coin.coin->output.value, genesis_tx.outputs.front().value);
    EXPECT_EQ(coin.coin->height, 0U);
    EXPECT_TRUE(coin.coin->is_coinbase);
    EXPECT_EQ(session->MineBlock({}, root / "mine", passphrase).error,
              nova::node::DurableSessionError::kMining);
    EXPECT_FALSE(std::filesystem::exists(root / "mine"));
    const auto address = session->GenerateAddress(root / "address", passphrase);
    ASSERT_EQ(address.error, nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(address.address);
    session.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(nova::node::DurableSession::Open(std::move(store),
                                               nova::consensus::NetworkId::kRegtest, time,
                                               start_time, passphrase),
              nullptr);
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(std::move(store), network.id, time, start_time,
                                               passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, network.genesis_hash);
    EXPECT_EQ(session->View()->receiving_address_count, 1U);
    const auto recovered_coin = session->ReadCoin(key);
    ASSERT_EQ(recovered_coin.error, nova::chain::CoinReadError::kNone);
    ASSERT_TRUE(recovered_coin.coin);
    EXPECT_EQ(recovered_coin.coin->output.value, genesis_tx.outputs.front().value);
    EXPECT_EQ(recovered_coin.coin->height, 0U);
    EXPECT_TRUE(recovered_coin.coin->is_coinbase);
    std::vector<std::uint8_t> script{0x76U, 0xA9U, 0x14U};
    script.insert(script.end(), address.address->public_key_hash.begin(),
                  address.address->public_key_hash.end());
    script.insert(script.end(), {0x88U, 0xACU});
    const auto subsidy = nova::consensus::GetBlockSubsidy(1U, network.monetary);
    ASSERT_TRUE(subsidy);
    nova::primitives::Transaction coinbase{
        1,
        {{nova::primitives::OutPoint{nova::crypto::Hash256{},
                                     std::numeric_limits<std::uint32_t>::max()},
          nova::node::CoinbaseHeightScript(1U), 0U}},
        {{*subsidy, script}},
        0U};
    nova::primitives::Block child{
        {1, network.genesis_hash, {}, start_time, network.pow.pow_limit_compact, 0U}, {coinbase}};
    const auto merkle = nova::primitives::ComputeMerkleRoot(
        child.transactions, network.block_limits.transaction_limits);
    ASSERT_TRUE(merkle);
    child.header.merkle_root = *merkle;
    std::optional<nova::crypto::Hash256> child_hash;
    for (std::uint32_t nonce = 0U; nonce < 4096U; ++nonce) {
        child.header.nonce = nonce;
        const auto hash = nova::primitives::ComputeBlockHash(child.header);
        if (hash && nova::consensus::CheckProofOfWork(*hash, network.pow.pow_limit)) {
            child_hash = hash;
            break;
        }
    }
    ASSERT_TRUE(child_hash);
    const auto child_id = coinbase.TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(child_id);
    const nova::chain::UTXOKey child_key{*child_id, 0U};
    auto invalid = child;
    invalid.header.merkle_root = {};
    EXPECT_EQ(session->ApplyBlock(invalid, {}, root / "invalid", passphrase).error,
              nova::node::DurableSessionError::kChain);
    EXPECT_EQ(session->View()->tip, network.genesis_hash);
    EXPECT_FALSE(session->ReadCoin(child_key).coin);
    {
        nova::wallet::detail::ScopedSensitiveFailureForTesting failure{
            nova::wallet::detail::SensitiveFailureStage::kFileStaging};
        EXPECT_EQ(session->ApplyBlock(child, {}, root / "wallet-failure", passphrase).error,
                  nova::node::DurableSessionError::kWallet);
    }
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, network.genesis_hash);
    EXPECT_FALSE(session->ReadCoin(child_key).coin);
    session.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(std::move(store), network.id, time, start_time,
                                               passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, network.genesis_hash);
    EXPECT_FALSE(session->ReadCoin(child_key).coin);
    EXPECT_EQ(session->ApplyBlock(child, {}, root / "child", passphrase).error,
              nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, *child_hash);
    EXPECT_EQ(session->View()->height, 1U);
    const auto child_coin = session->ReadCoin(child_key);
    ASSERT_EQ(child_coin.error, nova::chain::CoinReadError::kNone);
    ASSERT_TRUE(child_coin.coin);
    EXPECT_EQ(child_coin.coin->output.value, *subsidy);
    EXPECT_EQ(child_coin.coin->height, 1U);
    EXPECT_EQ(session->View()->confirmed_balance, *subsidy);
    session.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(std::move(store), network.id, time, start_time,
                                               passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, *child_hash);
    EXPECT_EQ(session->View()->height, 1U);
    EXPECT_EQ(session->View()->confirmed_balance, *subsidy);
    const auto restarted_child = session->ReadCoin(child_key);
    ASSERT_EQ(restarted_child.error, nova::chain::CoinReadError::kNone);
    ASSERT_TRUE(restarted_child.coin);
    EXPECT_EQ(restarted_child.coin->output.value, *subsidy);
    const auto subsidy_two = nova::consensus::GetBlockSubsidy(2U, network.monetary);
    ASSERT_TRUE(subsidy_two);
    nova::primitives::Transaction second_coinbase{
        1,
        {{nova::primitives::OutPoint{nova::crypto::Hash256{},
                                     std::numeric_limits<std::uint32_t>::max()},
          nova::node::CoinbaseHeightScript(2U), 0U}},
        {{*subsidy_two, script}},
        0U};
    nova::primitives::Block second{
        {1, *child_hash, {}, start_time + 1U, network.pow.pow_limit_compact, 0U},
        {second_coinbase}};
    const auto second_merkle = nova::primitives::ComputeMerkleRoot(
        second.transactions, network.block_limits.transaction_limits);
    ASSERT_TRUE(second_merkle);
    second.header.merkle_root = *second_merkle;
    std::optional<nova::crypto::Hash256> second_hash;
    for (std::uint32_t nonce = 0U; nonce < 4096U; ++nonce) {
        second.header.nonce = nonce;
        const auto hash = nova::primitives::ComputeBlockHash(second.header);
        if (hash && nova::consensus::CheckProofOfWork(*hash, network.pow.pow_limit)) {
            second_hash = hash;
            break;
        }
    }
    ASSERT_TRUE(second_hash);
    const auto second_id = second_coinbase.TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(second_id);
    const nova::chain::UTXOKey second_key{*second_id, 0U};
    ASSERT_TRUE(std::filesystem::create_directory(root / "staging"));
    auto workspace = nova::node::StagingWorkspace::Open(root / "staging");
    const nova::rpc::RpcConfig config{
        "127.0.0.1", "user", "pass", {256U, 8192U, 4096U}, nova::consensus::NetworkId::kTestnet};
    auto* observer = session.get();
    auto runtime = nova::daemon::DurableRuntime::Create(std::move(session), std::move(workspace),
                                                        passphrase, config);
    ASSERT_NE(runtime, nullptr);
    observer->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterWallet);
    nova::node::PeerServiceTestAccess::Handle(runtime->Peer(),
                                              {nova::net::Command::kBlock, second});
    ASSERT_TRUE(runtime->View());
    EXPECT_EQ(runtime->View()->tip, *child_hash);
    EXPECT_FALSE(observer->ReadCoin(second_key).coin);
    EXPECT_TRUE(std::filesystem::is_empty(root / "staging"));
    nova::node::PeerServiceTestAccess::Handle(runtime->Peer(),
                                              {nova::net::Command::kBlock, second});
    ASSERT_TRUE(runtime->View());
    EXPECT_EQ(runtime->View()->tip, *second_hash);
    EXPECT_EQ(runtime->View()->height, 2U);
    EXPECT_EQ(runtime->View()->confirmed_balance, *subsidy + *subsidy_two);
    ASSERT_TRUE(observer->ReadCoin(second_key).coin);
    runtime.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(std::move(store), network.id, time, start_time,
                                               passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, *second_hash);
    EXPECT_EQ(session->View()->confirmed_balance, *subsidy + *subsidy_two);
    const auto restarted_second = session->ReadCoin(second_key);
    ASSERT_EQ(restarted_second.error, nova::chain::CoinReadError::kNone);
    ASSERT_TRUE(restarted_second.coin);
    EXPECT_EQ(restarted_second.coin->height, 2U);
    session.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurableSession, BlockAndMiningRequireCurrentStoreAuthority)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-durable-authority-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_TRUE(std::filesystem::create_directory(root / "store"));
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const std::vector<std::uint8_t> passphrase{'t', 'e', 's', 't'};
    nova::storage::GenerationError error{};
    auto store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    auto session = nova::node::DurableSession::CreateFresh(
        std::move(store), network.id, time, kStartTime, passphrase, root / "initial");
    ASSERT_NE(session, nullptr);
    const auto before = session->View();
    ASSERT_TRUE(before);
    session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_EQ(session->ApplyBlock(nova::primitives::Block{}, {}, root / "block", passphrase).error,
              nova::node::DurableSessionError::kUnavailable);
    EXPECT_FALSE(session->View());
    EXPECT_FALSE(std::filesystem::exists(root / "block"));
    session.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(std::move(store), network.id, time, kStartTime,
                                               passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, before->tip);
    session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_EQ(session->MineBlock({}, root / "mine", passphrase).error,
              nova::node::DurableSessionError::kUnavailable);
    EXPECT_FALSE(session->View());
    EXPECT_FALSE(std::filesystem::exists(root / "mine"));
    session.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(std::move(store), network.id, time, kStartTime,
                                               passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, before->tip);
    session.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurablePeer, WalletImageFailureDoesNotPublishReceivedBlockAndRestartPreservesTip)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-durable-received-failure-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_TRUE(std::filesystem::create_directory(root / "store"));
    ASSERT_TRUE(std::filesystem::create_directory(root / "staging"));
    auto producer = RegtestNode::Create(Config(root, "producer", 29101U, 29102U));
    ASSERT_NE(producer, nullptr);
    const auto mined = producer->MineBlock();
    ASSERT_TRUE(mined.block_hash.has_value());
    const auto index = producer->chain_state().GetBlockIndex(*mined.block_hash);
    ASSERT_TRUE(index && index->block);
    const auto block = *index->block;
    const auto& network = nova::consensus::RegtestNetworkParams();
    ASSERT_FALSE(block.transactions.empty());
    const auto coinbase_id =
        block.transactions.front().TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(coinbase_id.has_value());
    const nova::chain::UTXOKey candidate_coin{*coinbase_id, 0U};
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const std::vector<std::uint8_t> passphrase{'t', 'e', 's', 't'};
    nova::storage::GenerationError error{};
    auto store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    auto session = nova::node::DurableSession::CreateFresh(
        std::move(store), network.id, time, kStartTime, passphrase, root / "initial");
    ASSERT_NE(session, nullptr);
    auto* session_observer = session.get();
    auto workspace = nova::node::StagingWorkspace::Open(root / "staging");
    const nova::rpc::RpcConfig config{
        "127.0.0.1", "user", "pass", {256U, 8192U, 4096U}, nova::consensus::NetworkId::kRegtest};
    auto runtime = nova::daemon::DurableRuntime::Create(std::move(session), std::move(workspace),
                                                        passphrase, config);
    ASSERT_NE(runtime, nullptr);
    const auto before = runtime->View();
    ASSERT_TRUE(before);
    const auto before_coin = session_observer->ReadCoin(candidate_coin);
    ASSERT_EQ(before_coin.error, nova::chain::CoinReadError::kNone);
    ASSERT_FALSE(before_coin.coin.has_value());
    session_observer->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterWallet);
    nova::node::PeerServiceTestAccess::Handle(runtime->Peer(), {nova::net::Command::kBlock, block});
    ASSERT_TRUE(runtime->View());
    EXPECT_EQ(runtime->View()->tip, before->tip);
    EXPECT_EQ(runtime->View()->height, before->height);
    EXPECT_EQ(runtime->View()->confirmed_balance, before->confirmed_balance);
    EXPECT_EQ(runtime->View()->chain_work, before->chain_work);
    EXPECT_EQ(runtime->View()->mempool_size, before->mempool_size);
    EXPECT_EQ(runtime->View()->mempool_bytes, before->mempool_bytes);
    const auto failed_coin = session_observer->ReadCoin(candidate_coin);
    EXPECT_EQ(failed_coin.error, nova::chain::CoinReadError::kNone);
    EXPECT_FALSE(failed_coin.coin.has_value());
    EXPECT_TRUE(std::filesystem::is_empty(root / "staging"));
    runtime.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(std::move(store), network.id, time, kStartTime,
                                               passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View());
    EXPECT_EQ(session->View()->tip, before->tip);
    EXPECT_EQ(session->View()->height, before->height);
    EXPECT_EQ(session->View()->confirmed_balance, before->confirmed_balance);
    EXPECT_EQ(session->View()->chain_work, before->chain_work);
    EXPECT_EQ(session->View()->mempool_size, before->mempool_size);
    EXPECT_EQ(session->View()->mempool_bytes, before->mempool_bytes);
    const auto restarted_coin = session->ReadCoin(candidate_coin);
    EXPECT_EQ(restarted_coin.error, nova::chain::CoinReadError::kNone);
    EXPECT_FALSE(restarted_coin.coin.has_value());
    session_observer = session.get();
    workspace = nova::node::StagingWorkspace::Open(root / "staging");
    runtime = nova::daemon::DurableRuntime::Create(std::move(session), std::move(workspace),
                                                   passphrase, config);
    ASSERT_NE(runtime, nullptr);
    nova::node::PeerServiceTestAccess::Handle(runtime->Peer(), {nova::net::Command::kBlock, block});
    ASSERT_TRUE(runtime->View());
    EXPECT_EQ(runtime->View()->tip, *mined.block_hash);
    EXPECT_EQ(runtime->View()->height, 1U);
    const auto accepted_coin = session_observer->ReadCoin(candidate_coin);
    EXPECT_EQ(accepted_coin.error, nova::chain::CoinReadError::kNone);
    EXPECT_TRUE(accepted_coin.coin.has_value());
    runtime.reset();
    producer.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurablePeer, RoutesBlockThroughOwnerAndQuarantinesFalseSuccess)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-durable-peer-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    ASSERT_TRUE(std::filesystem::create_directory(root / "store"));
    const auto& network = nova::consensus::RegtestNetworkParams();
    nova::storage::GenerationError error{};
    auto store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    const std::vector<std::uint8_t> passphrase{'t', 'e', 's', 't'};
    auto session = nova::node::DurableSession::CreateFresh(
        std::move(store), nova::consensus::NetworkId::kRegtest, time, kStartTime, passphrase,
        root / "initial");
    ASSERT_NE(session, nullptr);
    auto producer = RegtestNode::Create(Config(root, "producer", 29101U, 29102U));
    ASSERT_NE(producer, nullptr);
    const auto mined = producer->MineBlock();
    ASSERT_EQ(mined.error, RegtestNodeError::kNone);
    ASSERT_TRUE(mined.block_hash.has_value());
    const auto block = producer->chain_state().GetBlockIndex(*mined.block_hash);
    ASSERT_TRUE(block.has_value());
    ASSERT_TRUE(block->block.has_value());
    EXPECT_EQ(nova::node::PeerService::Create(
                  *session, std::function<std::optional<nova::node::DurablePeerWriteContext>()>{}),
              nullptr);
    auto unavailable_context = nova::node::PeerService::Create(
        *session,
        []() -> std::optional<nova::node::DurablePeerWriteContext> { return std::nullopt; });
    ASSERT_NE(unavailable_context, nullptr);
    nova::node::PeerServiceTestAccess::Handle(*unavailable_context,
                                              {nova::net::Command::kBlock, *block->block});
    EXPECT_TRUE(nova::node::PeerServiceTestAccess::Failed(*unavailable_context));
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->height, 0U);
    unavailable_context.reset();
    unsigned block_calls{};
    auto admit_block = [&](const nova::primitives::Block& candidate) {
        ++block_calls;
        return session->ApplyBlock(candidate, {}, root / "accepted-block", passphrase);
    };
    auto admit_transaction = [&](const nova::primitives::Transaction& candidate) {
        return session->AdmitTransaction(candidate, root / "accepted-transaction", passphrase);
    };
    EXPECT_EQ(nova::node::PeerService::Create(*session, {}, admit_transaction), nullptr);
    EXPECT_EQ(nova::node::PeerService::Create(*session, admit_block, {}), nullptr);
    auto service = nova::node::PeerService::Create(*session, admit_block, admit_transaction);
    ASSERT_NE(service, nullptr);
    nova::node::PeerServiceTestAccess::Handle(*service,
                                              {nova::net::Command::kBlock, *block->block});
    EXPECT_EQ(block_calls, 1U);
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->tip, *mined.block_hash);
    EXPECT_FALSE(nova::node::PeerServiceTestAccess::Failed(*service));
    const auto recipient = session->GenerateAddress(root / "peer-address", passphrase);
    ASSERT_EQ(recipient.error, nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(recipient.address.has_value());
    const std::array<nova::wallet::Recipient, 1U> recipients{
        nova::wallet::Recipient{recipient.address->public_key_hash, 1}};
    const auto payment = producer->CreateAndBroadcast(recipients);
    ASSERT_EQ(payment.error, RegtestNodeError::kNone);
    ASSERT_TRUE(payment.transaction.has_value());
    const auto transaction_id = payment.transaction->TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(transaction_id.has_value());
    nova::node::PeerServiceTestAccess::Handle(*service,
                                              {nova::net::Command::kTx, *payment.transaction});
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->mempool_size, 1U);
    EXPECT_TRUE(session->FindMempoolEntry(*transaction_id).entry.has_value());
    EXPECT_FALSE(nova::node::PeerServiceTestAccess::Failed(*service));
    nova::rpc::RpcConfig rpc_config{
        "127.0.0.1", "user", "pass", {256U, 8192U, 4096U}, nova::consensus::NetworkId::kRegtest};
    nova::rpc::DurableRpcActions actions;
    actions.submit_transaction = [&](const nova::primitives::Transaction& transaction) {
        return session->AdmitTransaction(transaction, root / "rpc-transaction", passphrase) ==
               nova::node::DurableSessionError::kNone;
    };
    actions.generate_address = [&]() -> std::optional<nova::wallet::WalletAddress> {
        const auto result = session->GenerateAddress(root / "rpc-address", passphrase);
        return result.error == nova::node::DurableSessionError::kNone ? result.address
                                                                      : std::nullopt;
    };
    auto wrong_network = rpc_config;
    wrong_network.network = nova::consensus::NetworkId::kTestnet;
    EXPECT_EQ(nova::rpc::CreateDurableRpcService(wrong_network, *session, service->peer_manager(),
                                                 actions),
              nullptr);
    auto rpc =
        nova::rpc::CreateDurableRpcService(rpc_config, *session, service->peer_manager(), actions);
    ASSERT_NE(rpc, nullptr);
    constexpr std::string_view auth{"Basic dXNlcjpwYXNz"};
    const auto chain_response = rpc->HandleHttpPost(
        {"POST", auth, R"({"jsonrpc":"2.0","id":1,"method":"getblockchaininfo"})"});
    EXPECT_NE(chain_response.body.find("\"blocks\":1"), std::string::npos);
    const auto pool_response = rpc->HandleHttpPost(
        {"POST", auth, R"({"jsonrpc":"2.0","id":2,"method":"getmempoolinfo"})"});
    EXPECT_NE(pool_response.body.find("\"size\":1"), std::string::npos);
    const auto wallet_response =
        rpc->HandleHttpPost({"POST", auth, R"({"jsonrpc":"2.0","id":3,"method":"getbalances"})"});
    EXPECT_NE(wallet_response.body.find("\"unconfirmed\""), std::string::npos);
    const auto address_response =
        rpc->HandleHttpPost({"POST", auth, R"({"jsonrpc":"2.0","id":4,"method":"getnewaddress"})"});
    EXPECT_EQ(address_response.body.find("\"error\""), std::string::npos);
    EXPECT_EQ(session->View()->receiving_address_count, 2U);
    session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    const auto unavailable = rpc->HandleHttpPost(
        {"POST", auth, R"({"jsonrpc":"2.0","id":5,"method":"getblockchaininfo"})"});
    EXPECT_NE(unavailable.body.find("node state unavailable"), std::string::npos);
    rpc.reset();
    service.reset();
    const auto next = producer->MineBlock();
    ASSERT_TRUE(next.block_hash.has_value());
    const auto next_block = producer->chain_state().GetBlockIndex(*next.block_hash);
    ASSERT_TRUE(next_block.has_value());
    ASSERT_TRUE(next_block->block.has_value());
    auto false_success = [](const nova::primitives::Block&) {
        return nova::node::DurableSessionResult{nova::node::DurableSessionError::kNone, {}};
    };
    auto liar = nova::node::PeerService::Create(*session, false_success, admit_transaction);
    ASSERT_NE(liar, nullptr);
    nova::node::PeerServiceTestAccess::Handle(*liar,
                                              {nova::net::Command::kBlock, *next_block->block});
    EXPECT_TRUE(nova::node::PeerServiceTestAccess::Failed(*liar));
    EXPECT_EQ(session->View()->tip, *mined.block_hash);
    EXPECT_EQ(session->View()->mempool_size, 1U);
    EXPECT_TRUE(session->FindMempoolEntry(*transaction_id).entry.has_value());
    liar.reset();
    auto altered_body = *block->block;
    ASSERT_FALSE(altered_body.transactions.empty());
    ASSERT_FALSE(altered_body.transactions.front().outputs.empty());
    ++altered_body.transactions.front().outputs.front().value;
    auto same_header_liar =
        nova::node::PeerService::Create(*session, false_success, admit_transaction);
    ASSERT_NE(same_header_liar, nullptr);
    nova::node::PeerServiceTestAccess::Handle(*same_header_liar,
                                              {nova::net::Command::kBlock, altered_body});
    EXPECT_TRUE(nova::node::PeerServiceTestAccess::Failed(*same_header_liar));
    EXPECT_EQ(session->View()->tip, *mined.block_hash);
    same_header_liar.reset();
    bool cleanup_called = false;
    const std::vector<std::uint8_t> wrong_passphrase{'n', 'o'};
    auto failed_cleanup = nova::node::PeerService::Create(
        *session, [&]() -> std::optional<nova::node::DurablePeerWriteContext> {
            return nova::node::DurablePeerWriteContext{root / "failed-cleanup-stage",
                                                       wrong_passphrase, [&]() {
                                                           cleanup_called = true;
                                                           return false;
                                                       }};
        });
    ASSERT_NE(failed_cleanup, nullptr);
    nova::node::PeerServiceTestAccess::Handle(*failed_cleanup,
                                              {nova::net::Command::kBlock, *next_block->block});
    EXPECT_TRUE(cleanup_called);
    EXPECT_TRUE(nova::node::PeerServiceTestAccess::Failed(*failed_cleanup));
    EXPECT_EQ(session->View()->tip, *mined.block_hash);
    failed_cleanup.reset();
    session.reset();
    store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    session = nova::node::DurableSession::Open(
        std::move(store), nova::consensus::NetworkId::kRegtest, time, kStartTime, passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->tip, *mined.block_hash);
    EXPECT_EQ(session->View()->mempool_size, 1U);
    EXPECT_TRUE(session->FindMempoolEntry(*transaction_id).entry.has_value());
    session.reset();
    producer.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestRecovery, InvalidReplayDoesNotRewriteExistingWallet)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-replay-wallet-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    const auto config = Config(root, "node", 29111U, 29112U);
    auto node = RegtestNode::Create(config);
    ASSERT_NE(node, nullptr);
    const auto mined = node->MineBlock();
    ASSERT_EQ(mined.error, RegtestNodeError::kNone);
    ASSERT_TRUE(mined.block_hash.has_value());
    const auto block = node->chain_state().GetBlockIndex(*mined.block_hash);
    ASSERT_TRUE(block.has_value());
    ASSERT_TRUE(block->block.has_value());
    node.reset();
    node = RegtestNode::Create(config);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->tip(), *mined.block_hash);
    node.reset();
    const auto wallet_path = config.data_directory / "wallet.dat";
    std::ifstream before_stream{wallet_path, std::ios::binary};
    ASSERT_TRUE(before_stream.is_open());
    const std::string before{std::istreambuf_iterator<char>{before_stream}, {}};
    ASSERT_FALSE(before.empty());
    before_stream.close();
    auto journal = nova::storage::BlockJournal::Open(
        config.data_directory, nova::consensus::RegtestNetworkParams().block_limits);
    ASSERT_TRUE(journal.has_value());
    ASSERT_EQ(journal->Prepare(*block->block), nova::storage::BlockJournalError::kNone);
    ASSERT_EQ(journal->Commit(*mined.block_hash), nova::storage::BlockJournalError::kNone);
    journal.reset();
    EXPECT_EQ(RegtestNode::Create(config), nullptr);
    std::ifstream after_stream{wallet_path, std::ios::binary};
    ASSERT_TRUE(after_stream.is_open());
    const std::string after{std::istreambuf_iterator<char>{after_stream}, {}};
    EXPECT_EQ(after, before);
    after_stream.close();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestAtomicityEvidence, PersistentLegacyStartupRejectsMissingJournalOrWallet)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-partial-legacy-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    const auto original_config = Config(root, "original", 29111U, 29112U);
    auto original = RegtestNode::Create(original_config);
    ASSERT_NE(original, nullptr);
    ASSERT_EQ(original->MineBlock().error, RegtestNodeError::kNone);
    const auto original_tip = original->tip();
    original.reset();
    original = RegtestNode::Create(original_config);
    ASSERT_NE(original, nullptr);
    EXPECT_EQ(original->tip(), original_tip);
    original.reset();
    const auto read = [](const auto& path) {
        std::ifstream file{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, {}};
    };
    for (const auto* missing : {"wallet.dat", "blocks.dat"}) {
        const auto config = Config(root, missing, 29113U, 29114U);
        ASSERT_TRUE(std::filesystem::create_directories(config.data_directory));
        std::filesystem::copy_file(original_config.data_directory / "network.identity",
                                   config.data_directory / "network.identity");
        const std::string_view retained =
            std::string_view{missing} == "wallet.dat" ? "blocks.dat" : "wallet.dat";
        std::filesystem::copy_file(original_config.data_directory / retained,
                                   config.data_directory / retained);
        const auto before = read(config.data_directory / retained);
        EXPECT_EQ(RegtestNode::Create(config), nullptr) << missing;
        EXPECT_FALSE(std::filesystem::exists(config.data_directory / missing)) << missing;
        EXPECT_EQ(read(config.data_directory / retained), before) << missing;
    }
    const auto ambiguous_config = Config(root, "both-missing", 29117U, 29118U);
    ASSERT_TRUE(std::filesystem::create_directories(ambiguous_config.data_directory));
    std::filesystem::copy_file(original_config.data_directory / "network.identity",
                               ambiguous_config.data_directory / "network.identity");
    const auto identity_before = read(ambiguous_config.data_directory / "network.identity");
    EXPECT_EQ(RegtestNode::Create(ambiguous_config), nullptr);
    EXPECT_FALSE(std::filesystem::exists(ambiguous_config.data_directory / "wallet.dat"));
    EXPECT_FALSE(std::filesystem::exists(ambiguous_config.data_directory / "blocks.dat"));
    EXPECT_EQ(read(ambiguous_config.data_directory / "network.identity"), identity_before);
    const auto mismatch_config = Config(root, "mismatched-pair", 29119U, 29120U);
    ASSERT_TRUE(std::filesystem::create_directories(mismatch_config.data_directory));
    std::filesystem::copy_file(original_config.data_directory / "network.identity",
                               mismatch_config.data_directory / "network.identity");
    std::filesystem::copy_file(original_config.data_directory / "wallet.dat",
                               mismatch_config.data_directory / "wallet.dat");
    {
        std::ofstream empty{mismatch_config.data_directory / "blocks.dat", std::ios::binary};
        ASSERT_TRUE(empty);
    }
    const auto wallet_before = read(mismatch_config.data_directory / "wallet.dat");
    EXPECT_EQ(RegtestNode::Create(mismatch_config), nullptr);
    EXPECT_EQ(read(mismatch_config.data_directory / "wallet.dat"), wallet_before);
    EXPECT_EQ(std::filesystem::file_size(mismatch_config.data_directory / "blocks.dat"), 0U);
    auto ephemeral_config = Config(root, "ephemeral", 29115U, 29116U);
    ephemeral_config.wallet_passphrase.clear();
    auto ephemeral = RegtestNode::Create(ephemeral_config);
    ASSERT_NE(ephemeral, nullptr);
    ephemeral.reset();
    EXPECT_FALSE(std::filesystem::exists(ephemeral_config.data_directory / "wallet.dat"));
    ephemeral = RegtestNode::Create(ephemeral_config);
    ASSERT_NE(ephemeral, nullptr);
    ephemeral.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestAtomicityEvidence, PersistentLegacyStartupRejectsNonemptyMismatchedPair)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-stale-legacy-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    const auto original_config = Config(root, "original", 29131U, 29132U);
    auto original = RegtestNode::Create(original_config);
    ASSERT_NE(original, nullptr);
    ASSERT_EQ(original->MineBlock().error, RegtestNodeError::kNone);
    const auto stale_config = Config(root, "stale", 29133U, 29134U);
    ASSERT_TRUE(std::filesystem::create_directories(stale_config.data_directory));
    std::filesystem::copy_file(original_config.data_directory / "network.identity",
                               stale_config.data_directory / "network.identity");
    std::filesystem::copy_file(original_config.data_directory / "blocks.dat",
                               stale_config.data_directory / "blocks.dat");
    ASSERT_EQ(original->MineBlock().error, RegtestNodeError::kNone);
    original.reset();
    std::filesystem::copy_file(original_config.data_directory / "wallet.dat",
                               stale_config.data_directory / "wallet.dat");
    const auto read = [](const auto& path) {
        std::ifstream file{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, {}};
    };
    const auto wallet_before = read(stale_config.data_directory / "wallet.dat");
    const auto journal_before = read(stale_config.data_directory / "blocks.dat");
    EXPECT_EQ(RegtestNode::Create(stale_config), nullptr);
    EXPECT_EQ(read(stale_config.data_directory / "wallet.dat"), wallet_before);
    EXPECT_EQ(read(stale_config.data_directory / "blocks.dat"), journal_before);
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestAtomicityEvidence, NodeOwnedAddressFailurePreservesIndexAndSuccessfulRetryRestarts)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-address-owner-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    const auto config = Config(root, "node", 29131U, 29132U);
    auto node = RegtestNode::Create(config);
    ASSERT_NE(node, nullptr);
    const auto prior_index = node->wallet().NextReceivingAddressIndex();
    const auto prior_count = node->wallet().ReceivingAddressCount();
    const auto prior_tip = node->tip();
    const auto wallet_path = config.data_directory / "wallet.dat";
    const auto preserved = config.data_directory / "wallet-preserved.dat";
    std::filesystem::rename(wallet_path, preserved);
    ASSERT_TRUE(std::filesystem::create_directory(wallet_path));
    EXPECT_FALSE(node->GenerateReceivingAddress().has_value());
    EXPECT_EQ(node->wallet().NextReceivingAddressIndex(), prior_index);
    EXPECT_EQ(node->wallet().ReceivingAddressCount(), prior_count);
    EXPECT_EQ(node->tip(), prior_tip);
    node.reset();
    ASSERT_TRUE(std::filesystem::remove(wallet_path));
    std::filesystem::rename(preserved, wallet_path);
    node = RegtestNode::Create(config);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->wallet().NextReceivingAddressIndex(), prior_index);
    EXPECT_EQ(node->wallet().ReceivingAddressCount(), prior_count);
    const auto address = node->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    EXPECT_EQ(address->index, prior_index);
    EXPECT_EQ(node->wallet().NextReceivingAddressIndex(), prior_index + 1U);
    node.reset();
    node = RegtestNode::Create(config);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->wallet().ReceivingAddressCount(), prior_count + 1U);
    const auto addresses = node->wallet().ListReceivingAddresses();
    EXPECT_EQ(addresses.back().public_key_hash, address->public_key_hash);
    EXPECT_EQ(node->wallet().NextReceivingAddressIndex(), prior_index + 1U);
    EXPECT_EQ(node->tip(), prior_tip);
    node.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestAtomicityEvidence, FailedPaymentMustNotPublishMempoolOrRelay)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-payment-owner-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    const auto sender_config = Config(root, "sender", 29151U, 29152U);
    auto sender = RegtestNode::Create(sender_config);
    auto receiver = RegtestNode::Create(Config(root, "receiver", 29153U, 29154U));
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);
    ASSERT_TRUE(sender->Connect(*receiver));
    ASSERT_EQ(sender->MineBlock().error, RegtestNodeError::kNone);
    const auto recipient = receiver->GenerateReceivingAddress();
    ASSERT_TRUE(recipient);
    const std::array<nova::wallet::Recipient, 1U> recipients{{{recipient->public_key_hash, 1}}};
    const auto prior_tip = sender->tip();
    const auto wallet_checkpoint = sender->wallet().CaptureCheckpoint();
    const auto wallet_path = sender_config.data_directory / "wallet.dat";
    const auto preserved = sender_config.data_directory / "wallet-preserved.dat";
    std::filesystem::rename(wallet_path, preserved);
    ASSERT_TRUE(std::filesystem::create_directory(wallet_path));
    const auto failed = sender->CreateAndBroadcast(recipients);
    EXPECT_NE(failed.error, RegtestNodeError::kNone);
    EXPECT_FALSE(failed.transaction);
    EXPECT_EQ(sender->mempool_size(), 0U);
    EXPECT_EQ(receiver->mempool_size(), 0U);
    EXPECT_TRUE(sender->wallet().IsCheckpointCurrent(wallet_checkpoint));
    EXPECT_EQ(sender->tip(), prior_tip);
    ASSERT_TRUE(std::filesystem::remove(wallet_path));
    std::filesystem::rename(preserved, wallet_path);
    const auto retried = sender->CreateAndBroadcast(recipients);
    ASSERT_EQ(retried.error, RegtestNodeError::kNone);
    ASSERT_TRUE(retried.transaction);
    EXPECT_EQ(sender->mempool_size(), 1U);
    EXPECT_EQ(receiver->mempool_size(), 1U);
    sender->DisconnectAll();
    sender.reset();
    receiver.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestAtomicityEvidence, WalletPersistenceFailureMustNotPublishChainTransition)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-atomicity-evidence-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    const auto receiver_config = Config(root, "receiver", 29121U, 29122U);
    auto receiver = RegtestNode::Create(receiver_config);
    auto producer = RegtestNode::Create(Config(root, "producer", 29123U, 29124U));
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(producer, nullptr);
    const auto mined = producer->MineBlock();
    ASSERT_EQ(mined.error, RegtestNodeError::kNone);
    ASSERT_TRUE(mined.block_hash.has_value());
    const auto index = producer->chain_state().GetBlockIndex(*mined.block_hash);
    ASSERT_TRUE(index.has_value());
    ASSERT_TRUE(index->block.has_value());
    const auto old_tip = receiver->tip();
    const auto old_height = receiver->height();
    const auto wallet_path = receiver_config.data_directory / "wallet.dat";
    const auto preserved = receiver_config.data_directory / "wallet-preserved.dat";
    ASSERT_EQ(std::filesystem::canonical(wallet_path).parent_path(),
              std::filesystem::canonical(receiver_config.data_directory));
    ASSERT_EQ(preserved.parent_path(), wallet_path.parent_path());
    std::filesystem::rename(wallet_path, preserved);
    ASSERT_TRUE(std::filesystem::create_directory(wallet_path));
    const auto result = receiver->ReceiveBlock(*index->block);
    EXPECT_EQ(result, RegtestNodeError::kWalletFailure);
    // This is the required failure non-publication oracle, not a description
    // of existing behavior. Preserve its failure before designing a remedy.
    EXPECT_EQ(receiver->height(), old_height);
    EXPECT_EQ(receiver->tip(), old_tip);
    const auto live_tip = receiver->tip();
    receiver.reset();
    ASSERT_TRUE(std::filesystem::remove(wallet_path)); // task-owned empty directory only
    std::filesystem::rename(preserved, wallet_path);
    auto restarted = RegtestNode::Create(receiver_config);
    ASSERT_NE(restarted, nullptr);
    EXPECT_EQ(restarted->tip(), live_tip);
    EXPECT_EQ(restarted->height(), old_height); // no failed operation may reappear on restart
    restarted.reset();
    producer.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root); // unique directory created exclusively by this fixture
}

TEST(RegtestPreparedBlock, PreparesAllComponentsWithoutPublishingAndRejectsMempoolFailure)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-prepared-block-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    auto receiver = RegtestNode::Create(Config(root, "receiver", 29221U, 29222U));
    auto producer = RegtestNode::Create(Config(root, "producer", 29223U, 29224U));
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(producer, nullptr);
    const auto mined = producer->MineBlock();
    ASSERT_EQ(mined.error, RegtestNodeError::kNone);
    ASSERT_TRUE(mined.block_hash.has_value());
    const auto index = producer->chain_state().GetBlockIndex(*mined.block_hash);
    ASSERT_TRUE(index.has_value());
    ASSERT_TRUE(index->block.has_value());
    const auto old_tip = receiver->tip();
    const auto old_height = receiver->height();
    const auto old_time = receiver->next_mining_time();
    const auto old_coins = receiver->utxos().size();
    const auto old_balance = receiver->wallet().ConfirmedBalance();
    auto result = nova::node::PrepareNodeBlock(receiver->chain_state(), receiver->wallet(),
                                               receiver->mempool(), *index->block, {}, old_time);
    ASSERT_EQ(result.error, nova::node::BlockPreparationError::kNone);
    ASSERT_NE(result.candidate, nullptr);
    EXPECT_TRUE(result.candidate->CanPublish(receiver->chain_state(), receiver->wallet(),
                                             receiver->mempool(), old_time));
    EXPECT_FALSE(result.candidate->CanPublish(producer->chain_state(), receiver->wallet(),
                                              receiver->mempool(), old_time));
    EXPECT_FALSE(result.candidate->CanPublish(receiver->chain_state(), receiver->wallet(),
                                              receiver->mempool(), old_time + 1U));
    EXPECT_EQ(receiver->tip(), old_tip);
    EXPECT_EQ(receiver->height(), old_height);
    EXPECT_EQ(receiver->utxos().size(), old_coins);
    EXPECT_EQ(receiver->wallet().ConfirmedBalance(), old_balance);
    EXPECT_EQ(receiver->next_mining_time(), old_time);
    EXPECT_EQ(receiver->mempool_size(), 0U);
    result.candidate.reset();
    auto invalid = *index->block;
    invalid.transactions.clear();
    const auto rejected = nova::node::PrepareNodeBlock(receiver->chain_state(), receiver->wallet(),
                                                       receiver->mempool(), invalid, {}, old_time);
    EXPECT_EQ(rejected.error, nova::node::BlockPreparationError::kChain);
    EXPECT_EQ(rejected.candidate, nullptr);
    auto maximum_time = *index->block;
    maximum_time.header.time = std::numeric_limits<std::uint32_t>::max();
    const auto boundary =
        nova::node::PrepareNodeBlock(receiver->chain_state(), receiver->wallet(),
                                     receiver->mempool(), maximum_time, {}, old_time);
    EXPECT_EQ(boundary.error, nova::node::BlockPreparationError::kContext);
    EXPECT_EQ(boundary.candidate, nullptr);
    auto address = receiver->wallet().PrepareReceivingAddress();
    ASSERT_NE(address, nullptr);
    auto paying = *index->block;
    paying.transactions.front().outputs.front().script_pubkey =
        P2pkhScript(address->address().public_key_hash);
    const auto root_hash = nova::primitives::ComputeMerkleRoot(
        paying.transactions, receiver->network_params().block_limits.transaction_limits);
    ASSERT_TRUE(root_hash.has_value());
    paying.header.merkle_root = *root_hash;
    const auto remined = nova::consensus::MineRegtestBlock(paying.header, 4'096U);
    ASSERT_TRUE(remined.has_value());
    paying.header = remined->header;
    auto joint =
        nova::node::PrepareNodeBlock(receiver->chain_state(), receiver->wallet(),
                                     receiver->mempool(), paying, {}, old_time, address.get());
    ASSERT_EQ(joint.error, nova::node::BlockPreparationError::kNone);
    ASSERT_NE(joint.candidate, nullptr);
    EXPECT_TRUE(joint.candidate->CanPublish(receiver->chain_state(), receiver->wallet(),
                                            receiver->mempool(), old_time, address.get()));
    EXPECT_FALSE(joint.candidate->CanPublish(receiver->chain_state(), receiver->wallet(),
                                             receiver->mempool(), old_time));
    EXPECT_TRUE(receiver->wallet().ListReceivingAddresses().empty());
    EXPECT_EQ(receiver->wallet().ConfirmedBalance(), old_balance);
    const auto store_directory = root / "generation";
    ASSERT_TRUE(std::filesystem::create_directory(store_directory));
    const auto base_journal = root / "base-journal.dat";
    {
        std::ofstream file{base_journal, std::ios::binary};
        ASSERT_TRUE(file.good());
    }
    const auto base_wallet = root / "base-wallet.dat";
    const std::vector<std::uint8_t> passphrase{'s', 't', 'a', 'g', 'e'};
    ASSERT_TRUE(receiver->wallet().SaveEncrypted(base_wallet, passphrase));
    nova::storage::GenerationError store_error{};
    auto store = nova::storage::GenerationStore::Open(store_directory, receiver->network_params(),
                                                      store_error);
    ASSERT_NE(store, nullptr);
    auto base = store->Prepare(base_journal, base_wallet, store_error);
    ASSERT_NE(base, nullptr);
    ASSERT_EQ(store->Commit(*base).state, nova::storage::GenerationCommitState::kCommitted);
    const auto before = store->Observe();
    ASSERT_EQ(before.state, nova::storage::GenerationState::kPresent);
    const auto baseline_time_source = std::make_shared<nova::chain::SystemValidationTimeSource>();
    auto baseline = nova::node::RecoverGeneration(*store, nova::consensus::NetworkId::kRegtest,
                                                  baseline_time_source, kStartTime, passphrase);
    ASSERT_EQ(baseline.error, nova::node::GenerationRecoveryError::kNone);
    ASSERT_NE(baseline.recovered, nullptr);
    EXPECT_TRUE(baseline.recovered->MatchesStore(*store));
    auto guarded_address = baseline.recovered->wallet().PrepareReceivingAddress();
    ASSERT_NE(guarded_address, nullptr);
    auto guarded_block = *index->block;
    guarded_block.transactions.front().outputs.front().script_pubkey =
        P2pkhScript(guarded_address->address().public_key_hash);
    const auto guarded_root = nova::primitives::ComputeMerkleRoot(
        guarded_block.transactions, receiver->network_params().block_limits.transaction_limits);
    ASSERT_TRUE(guarded_root.has_value());
    guarded_block.header.merkle_root = *guarded_root;
    const auto guarded_mined = nova::consensus::MineRegtestBlock(guarded_block.header, 4'096U);
    ASSERT_TRUE(guarded_mined.has_value());
    guarded_block.header = guarded_mined->header;
    auto guarded = nova::node::PrepareNodeBlock(baseline.recovered->chain(),
                                                baseline.recovered->wallet(), receiver->mempool(),
                                                guarded_block, {}, old_time, guarded_address.get());
    ASSERT_EQ(guarded.error, nova::node::BlockPreparationError::kNone);
    ASSERT_NE(guarded.candidate, nullptr);
    nova::node::GenerationStageError guarded_error{};
    const std::vector<std::uint8_t> bad_staging_pass{'w', 'r', 'o', 'n', 'g'};
    auto wrong_pass_stage = guarded.candidate->StageRecoveredGeneration(
        *store, *baseline.recovered, receiver->mempool(), old_time, root / "wrong-pass-stage",
        bad_staging_pass, guarded_error, guarded_address.get());
    EXPECT_EQ(wrong_pass_stage, nullptr);
    EXPECT_EQ(guarded_error, nova::node::GenerationStageError::kWallet);
    EXPECT_FALSE(std::filesystem::exists(root / "wrong-pass-stage"));
    EXPECT_EQ(store->Observe().snapshot.sequence, before.snapshot.sequence);
    store->FailNextForTesting(nova::storage::GenerationFailurePoint::kAfterWallet);
    auto failed_stage = guarded.candidate->StageRecoveredGeneration(
        *store, *baseline.recovered, receiver->mempool(), old_time, root / "failed-stage",
        passphrase, guarded_error, guarded_address.get());
    EXPECT_EQ(failed_stage, nullptr);
    EXPECT_EQ(guarded_error, nova::node::GenerationStageError::kStore);
    EXPECT_EQ(store->Observe().snapshot.sequence, before.snapshot.sequence);
    auto guarded_stage = guarded.candidate->StageRecoveredGeneration(
        *store, *baseline.recovered, receiver->mempool(), old_time, root / "guarded-stage",
        passphrase, guarded_error, guarded_address.get());
    ASSERT_NE(guarded_stage, nullptr);
    EXPECT_EQ(store->Observe().snapshot.sequence, before.snapshot.sequence);
    auto other_candidate = nova::node::PrepareNodeBlock(
        baseline.recovered->chain(), baseline.recovered->wallet(), receiver->mempool(),
        guarded_block, {}, old_time, guarded_address.get());
    ASSERT_NE(other_candidate.candidate, nullptr);
    EXPECT_EQ(other_candidate.candidate->CommitGeneration(*guarded_stage, *store,
                                                          *baseline.recovered, receiver->mempool(),
                                                          guarded_address.get()),
              nova::node::NodePublicationResult::kStale);
    store->FailNextForTesting(nova::storage::GenerationFailurePoint::kBeforeRootReplace);
    EXPECT_EQ(guarded.candidate->CommitGeneration(*guarded_stage, *store, *baseline.recovered,
                                                  receiver->mempool(), guarded_address.get()),
              nova::node::NodePublicationResult::kNotCommitted);
    EXPECT_EQ(store->Observe().snapshot.sequence, before.snapshot.sequence);
    EXPECT_EQ(baseline.recovered->next_time(), old_time);
    std::ifstream staged_journal{root / "guarded-stage" / "blocks.dat", std::ios::binary};
    const auto records =
        nova::storage::ReadBlockJournal(staged_journal, baseline.recovered->block_limits());
    ASSERT_EQ(records.error, nova::storage::BlockJournalError::kNone);
    ASSERT_FALSE(records.recovery_required);
    ASSERT_EQ(records.blocks.size(), 1U);
    EXPECT_EQ(nova::primitives::ComputeBlockHash(records.blocks.front().header),
              nova::primitives::ComputeBlockHash(guarded_block.header));
    const nova::wallet::WalletParams wallet_params{
        baseline.recovered->block_limits().transaction_limits, 1, 1U, true};
    const auto loaded = nova::wallet::Wallet::LoadEncrypted(
        wallet_params, root / "guarded-stage" / "wallet.dat", passphrase);
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->ListReceivingAddresses().size(), 1U);
    EXPECT_EQ(loaded->ListReceivingAddresses().front().public_key_hash,
              guarded_address->address().public_key_hash);
    EXPECT_EQ(loaded->ConfirmedBalance(), guarded_block.transactions.front().outputs.front().value);
    staged_journal.close();
    auto stale_stage = guarded.candidate->StageRecoveredGeneration(
        *store, *baseline.recovered, receiver->mempool(), old_time, root / "stale-prepared",
        passphrase, guarded_error, guarded_address.get());
    ASSERT_NE(stale_stage, nullptr);
    ASSERT_TRUE(baseline.recovered->wallet().GenerateReceivingAddress().has_value());
    EXPECT_FALSE(baseline.recovered->StillMatchesBase(old_time));
    auto stale_guard = guarded.candidate->StageRecoveredGeneration(
        *store, *baseline.recovered, receiver->mempool(), old_time, root / "stale-guard-stage",
        passphrase, guarded_error, guarded_address.get());
    EXPECT_EQ(stale_guard, nullptr);
    EXPECT_EQ(guarded_error, nova::node::GenerationStageError::kStale);
    EXPECT_FALSE(std::filesystem::exists(root / "stale-guard-stage"));
    EXPECT_EQ(guarded.candidate->CommitGeneration(*stale_stage, *store, *baseline.recovered,
                                                  receiver->mempool(), guarded_address.get()),
              nova::node::NodePublicationResult::kStale);
    EXPECT_EQ(store->Observe().snapshot.sequence, before.snapshot.sequence);
    stale_stage.reset();
    guarded_stage.reset();
    other_candidate.candidate.reset();
    guarded.candidate.reset();
    guarded_address.reset();
    baseline.recovered.reset();
    store.reset();
    EXPECT_EQ(receiver->mempool().RemoveForBlock({}).error, nova::chain::MempoolError::kNone);
    EXPECT_FALSE(joint.candidate->CanPublish(receiver->chain_state(), receiver->wallet(),
                                             receiver->mempool(), old_time, address.get()));
    joint.candidate.reset();
    auto stale_wallet =
        nova::node::PrepareNodeBlock(receiver->chain_state(), receiver->wallet(),
                                     receiver->mempool(), *index->block, {}, old_time);
    ASSERT_EQ(stale_wallet.error, nova::node::BlockPreparationError::kNone);
    ASSERT_NE(stale_wallet.candidate, nullptr);
    EXPECT_FALSE(stale_wallet.candidate->CanPublish(receiver->chain_state(), producer->wallet(),
                                                    receiver->mempool(), old_time));
    ASSERT_TRUE(receiver->wallet().GenerateReceivingAddress().has_value());
    EXPECT_FALSE(stale_wallet.candidate->CanPublish(receiver->chain_state(), receiver->wallet(),
                                                    receiver->mempool(), old_time));
    stale_wallet.candidate.reset();
    receiver->mempool().SetFailurePointForTesting(
        nova::chain::Mempool::FailurePoint::kBeforePreparedCopy);
    auto failed = nova::node::PrepareNodeBlock(receiver->chain_state(), receiver->wallet(),
                                               receiver->mempool(), *index->block, {}, old_time);
    EXPECT_EQ(failed.error, nova::node::BlockPreparationError::kMempool);
    EXPECT_EQ(failed.candidate, nullptr);
    EXPECT_EQ(receiver->tip(), old_tip);
    EXPECT_EQ(receiver->height(), old_height);
    EXPECT_EQ(receiver->utxos().size(), old_coins);
    EXPECT_EQ(receiver->wallet().ConfirmedBalance(), old_balance);
    EXPECT_EQ(receiver->next_mining_time(), old_time);
    receiver.reset();
    producer.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestPreparedBlock, ResolvesPostRootFailureBeforePublishingPreparedState)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-prepared-commit-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    auto receiver = RegtestNode::Create(Config(root, "receiver", 29321U, 29322U));
    auto producer = RegtestNode::Create(Config(root, "producer", 29323U, 29324U));
    ASSERT_NE(receiver, nullptr);
    ASSERT_NE(producer, nullptr);
    const auto mined = producer->MineBlock();
    ASSERT_EQ(mined.error, RegtestNodeError::kNone);
    ASSERT_TRUE(mined.block_hash.has_value());
    const auto index = producer->chain_state().GetBlockIndex(*mined.block_hash);
    ASSERT_TRUE(index.has_value());
    ASSERT_TRUE(index->block.has_value());
    auto now = receiver->next_mining_time();
    ASSERT_TRUE(std::filesystem::create_directory(root / "generation"));
    const auto journal = root / "base-journal.dat";
    {
        std::ofstream file{journal, std::ios::binary};
        ASSERT_TRUE(file.good());
    }
    const std::vector<std::uint8_t> passphrase{'s', 't', 'a', 'g', 'e'};
    const auto wallet = root / "base-wallet.dat";
    ASSERT_TRUE(receiver->wallet().SaveEncrypted(wallet, passphrase));
    nova::storage::GenerationError store_error{};
    auto store = nova::storage::GenerationStore::Open(root / "generation",
                                                      receiver->network_params(), store_error);
    ASSERT_NE(store, nullptr);
    auto base = store->Prepare(journal, wallet, store_error);
    ASSERT_NE(base, nullptr);
    ASSERT_EQ(store->Commit(*base).state, nova::storage::GenerationCommitState::kCommitted);
    const auto before = store->Observe();
    ASSERT_EQ(before.state, nova::storage::GenerationState::kPresent);
    const auto time_source = std::make_shared<nova::chain::SystemValidationTimeSource>();
    auto baseline = nova::node::RecoverGeneration(*store, nova::consensus::NetworkId::kRegtest,
                                                  time_source, kStartTime, passphrase);
    ASSERT_EQ(baseline.error, nova::node::GenerationRecoveryError::kNone);
    ASSERT_NE(baseline.recovered, nullptr);
    auto prepared =
        nova::node::PrepareNodeBlock(baseline.recovered->chain(), baseline.recovered->wallet(),
                                     receiver->mempool(), *index->block, {}, now);
    ASSERT_EQ(prepared.error, nova::node::BlockPreparationError::kNone);
    ASSERT_NE(prepared.candidate, nullptr);
    nova::node::GenerationStageError stage_error{};
    auto staged = prepared.candidate->StageRecoveredGeneration(
        *store, *baseline.recovered, receiver->mempool(), now, root / "stage", passphrase,
        stage_error);
    ASSERT_NE(staged, nullptr);
    store->FailNextForTesting(nova::storage::GenerationFailurePoint::kAfterRootReplace);
    EXPECT_EQ(prepared.candidate->CommitGeneration(*staged, *store, *baseline.recovered,
                                                   receiver->mempool()),
              nova::node::NodePublicationResult::kCommitted);
    EXPECT_EQ(baseline.recovered->chain().active_height(), 1U);
    EXPECT_EQ(baseline.recovered->chain().active_tip(), *mined.block_hash);
    now = baseline.recovered->next_time();
    EXPECT_EQ(now, prepared.candidate->next_time());
    EXPECT_TRUE(baseline.recovered->StillMatchesBase(now));
    EXPECT_GT(store->Observe().snapshot.sequence, before.snapshot.sequence);
    EXPECT_EQ(prepared.candidate->CommitGeneration(*staged, *store, *baseline.recovered,
                                                   receiver->mempool()),
              nova::node::NodePublicationResult::kStale);
    const auto second_mined = producer->MineBlock();
    ASSERT_EQ(second_mined.error, RegtestNodeError::kNone);
    ASSERT_TRUE(second_mined.block_hash.has_value());
    const auto second_index = producer->chain_state().GetBlockIndex(*second_mined.block_hash);
    ASSERT_TRUE(second_index.has_value());
    ASSERT_TRUE(second_index->block.has_value());
    auto second =
        nova::node::PrepareNodeBlock(baseline.recovered->chain(), baseline.recovered->wallet(),
                                     receiver->mempool(), *second_index->block, {}, now);
    ASSERT_EQ(second.error, nova::node::BlockPreparationError::kNone);
    ASSERT_NE(second.candidate, nullptr);
    auto second_stage = second.candidate->StageRecoveredGeneration(
        *store, *baseline.recovered, receiver->mempool(), now, root / "second-stage", passphrase,
        stage_error);
    ASSERT_NE(second_stage, nullptr);
    ASSERT_EQ(second.candidate->CommitGeneration(*second_stage, *store, *baseline.recovered,
                                                 receiver->mempool()),
              nova::node::NodePublicationResult::kCommitted);
    now = baseline.recovered->next_time();
    EXPECT_TRUE(baseline.recovered->StillMatchesBase(now));
    EXPECT_EQ(baseline.recovered->chain().active_height(), 2U);
    auto side_block = *index->block;
    auto side_script = P2pkhScript(nova::crypto::Hash160{});
    if (side_script == side_block.transactions.front().outputs.front().script_pubkey)
        side_script[3] ^= 1U;
    side_block.transactions.front().outputs.front().script_pubkey = side_script;
    const auto side_root = nova::primitives::ComputeMerkleRoot(
        side_block.transactions, baseline.recovered->block_limits().transaction_limits);
    ASSERT_TRUE(side_root.has_value());
    side_block.header.merkle_root = *side_root;
    const auto side_mined = nova::consensus::MineRegtestBlock(side_block.header, 4'096U);
    ASSERT_TRUE(side_mined.has_value());
    side_block.header = side_mined->header;
    const auto prior_side = store->Observe();
    ASSERT_EQ(prior_side.state, nova::storage::GenerationState::kPresent);
    const auto prior_tip = baseline.recovered->chain().active_tip();
    const auto prior_known = baseline.recovered->chain().known_block_count();
    const auto prior_balance = baseline.recovered->wallet().ConfirmedBalance();
    const auto prior_mempool = receiver->mempool_size();
    auto side =
        nova::node::PrepareNodeBlock(baseline.recovered->chain(), baseline.recovered->wallet(),
                                     receiver->mempool(), side_block, {}, now);
    ASSERT_EQ(side.error, nova::node::BlockPreparationError::kNone);
    ASSERT_NE(side.candidate, nullptr);
    EXPECT_FALSE(side.candidate->result().became_active);
    auto side_stage =
        side.candidate->StageRecoveredGeneration(*store, *baseline.recovered, receiver->mempool(),
                                                 now, root / "side-stage", passphrase, stage_error);
    ASSERT_NE(side_stage, nullptr);
    ASSERT_EQ(side.candidate->CommitGeneration(*side_stage, *store, *baseline.recovered,
                                               receiver->mempool()),
              nova::node::NodePublicationResult::kCommitted);
    now = baseline.recovered->next_time();
    EXPECT_TRUE(baseline.recovered->StillMatchesBase(now));
    EXPECT_EQ(baseline.recovered->chain().active_tip(), prior_tip);
    EXPECT_EQ(baseline.recovered->chain().known_block_count(), prior_known + 1U);
    EXPECT_EQ(baseline.recovered->wallet().ConfirmedBalance(), prior_balance);
    EXPECT_EQ(receiver->mempool_size(), prior_mempool);
    const auto after_side = store->Observe();
    ASSERT_EQ(after_side.state, nova::storage::GenerationState::kPresent);
    EXPECT_EQ(after_side.snapshot.wallet_hash, prior_side.snapshot.wallet_hash);
    EXPECT_EQ(after_side.snapshot.wallet_size, prior_side.snapshot.wallet_size);
    auto recovered = nova::node::RecoverGeneration(*store, nova::consensus::NetworkId::kRegtest,
                                                   time_source, kStartTime, passphrase);
    ASSERT_EQ(recovered.error, nova::node::GenerationRecoveryError::kNone);
    ASSERT_NE(recovered.recovered, nullptr);
    EXPECT_EQ(recovered.recovered->chain().active_height(), 2U);
    EXPECT_EQ(recovered.recovered->chain().active_tip(), *second_mined.block_hash);
    EXPECT_TRUE(recovered.recovered->chain().GetBlockIndex(side_mined->block_hash).has_value());
    EXPECT_EQ(recovered.recovered->next_time(), now);
    EXPECT_TRUE(recovered.recovered->StillMatchesBase(now));
    EXPECT_FALSE(recovered.recovered->StillMatchesBase(now + 1U));
    ASSERT_TRUE(recovered.recovered->wallet().GenerateReceivingAddress().has_value());
    EXPECT_FALSE(recovered.recovered->StillMatchesBase(now));
    auto recovered_utxo = nova::node::RecoverGeneration(
        *store, nova::consensus::NetworkId::kRegtest, time_source, kStartTime, passphrase);
    ASSERT_EQ(recovered_utxo.error, nova::node::GenerationRecoveryError::kNone);
    ASSERT_NE(recovered_utxo.recovered, nullptr);
    EXPECT_TRUE(recovered_utxo.recovered->StillMatchesBase(now));
    const nova::chain::UTXOKey artificial{nova::crypto::Hash256{}, 42U};
    ASSERT_TRUE(recovered_utxo.recovered->utxos().AddCoin(
        artificial, nova::chain::Coin{nova::primitives::TxOutput{1, {}}, 0U, false}));
    EXPECT_FALSE(recovered_utxo.recovered->StillMatchesBase(now));
    recovered_utxo.recovered.reset();
    EXPECT_EQ(nova::node::RecoverGeneration(*store, nova::consensus::NetworkId::kTestnet,
                                            time_source, kStartTime, passphrase)
                  .error,
              nova::node::GenerationRecoveryError::kNetwork);
    const std::vector<std::uint8_t> wrong_pass{'w', 'r', 'o', 'n', 'g'};
    EXPECT_EQ(nova::node::RecoverGeneration(*store, nova::consensus::NetworkId::kRegtest,
                                            time_source, kStartTime, wrong_pass)
                  .error,
              nova::node::GenerationRecoveryError::kWallet);
    ASSERT_TRUE(std::filesystem::create_directory(root / "mismatch-generation"));
    const auto mismatch_wallet = root / "mismatch-wallet.dat";
    ASSERT_TRUE(producer->wallet().SaveEncrypted(mismatch_wallet, passphrase));
    auto mismatch_store = nova::storage::GenerationStore::Open(
        root / "mismatch-generation", receiver->network_params(), store_error);
    ASSERT_NE(mismatch_store, nullptr);
    EXPECT_FALSE(recovered.recovered->MatchesStore(*mismatch_store));
    auto mismatch = mismatch_store->Prepare(journal, mismatch_wallet, store_error);
    ASSERT_NE(mismatch, nullptr);
    ASSERT_EQ(mismatch_store->Commit(*mismatch).state,
              nova::storage::GenerationCommitState::kCommitted);
    EXPECT_EQ(nova::node::RecoverGeneration(*mismatch_store, nova::consensus::NetworkId::kRegtest,
                                            time_source, kStartTime, passphrase)
                  .error,
              nova::node::GenerationRecoveryError::kWallet);
    mismatch_store.reset();
    recovered.recovered.reset();
    const auto third_mined = producer->MineBlock();
    ASSERT_EQ(third_mined.error, RegtestNodeError::kNone);
    ASSERT_TRUE(third_mined.block_hash.has_value());
    const auto third_index = producer->chain_state().GetBlockIndex(*third_mined.block_hash);
    ASSERT_TRUE(third_index.has_value());
    ASSERT_TRUE(third_index->block.has_value());
    auto third =
        nova::node::PrepareNodeBlock(baseline.recovered->chain(), baseline.recovered->wallet(),
                                     receiver->mempool(), *third_index->block, {}, now);
    ASSERT_EQ(third.error, nova::node::BlockPreparationError::kNone);
    ASSERT_NE(third.candidate, nullptr);
    auto third_stage = third.candidate->StageRecoveredGeneration(
        *store, *baseline.recovered, receiver->mempool(), now, root / "third-stage", passphrase,
        stage_error);
    ASSERT_NE(third_stage, nullptr);
    baseline.recovered->FailNextAdvanceForTesting();
    EXPECT_EQ(third.candidate->CommitGeneration(*third_stage, *store, *baseline.recovered,
                                                receiver->mempool()),
              nova::node::NodePublicationResult::kUnavailable);
    EXPECT_FALSE(baseline.recovered->StillMatchesBase(now));
    EXPECT_EQ(third.candidate->CommitGeneration(*third_stage, *store, *baseline.recovered,
                                                receiver->mempool()),
              nova::node::NodePublicationResult::kUnavailable);
    auto recovered_after_failure = nova::node::RecoverGeneration(
        *store, nova::consensus::NetworkId::kRegtest, time_source, kStartTime, passphrase);
    ASSERT_EQ(recovered_after_failure.error, nova::node::GenerationRecoveryError::kNone);
    ASSERT_NE(recovered_after_failure.recovered, nullptr);
    EXPECT_EQ(recovered_after_failure.recovered->chain().active_height(), 3U);
    EXPECT_EQ(recovered_after_failure.recovered->chain().active_tip(), *third_mined.block_hash);
    recovered_after_failure.recovered.reset();
    third_stage.reset();
    third.candidate.reset();
    const auto authoritative = store->Observe();
    ASSERT_EQ(authoritative.state, nova::storage::GenerationState::kPresent);
    ASSERT_EQ(std::filesystem::canonical(authoritative.snapshot.journal).parent_path(),
              std::filesystem::canonical(root / "generation"));
    {
        std::ofstream corrupt{authoritative.snapshot.journal, std::ios::binary | std::ios::app};
        ASSERT_TRUE(corrupt.is_open());
        corrupt.put('\x01');
        ASSERT_TRUE(corrupt.good());
    }
    EXPECT_EQ(nova::node::RecoverGeneration(*store, nova::consensus::NetworkId::kRegtest,
                                            time_source, kStartTime, passphrase)
                  .error,
              nova::node::GenerationRecoveryError::kUnavailable);
    staged.reset();
    prepared.candidate.reset();
    second_stage.reset();
    second.candidate.reset();
    side_stage.reset();
    side.candidate.reset();
    baseline.recovered.reset();
    store.reset();
    receiver.reset();
    producer.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestGenerationMigration, ValidatesCopiedLegacyPairBeforeChangingAuthority)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-generation-migration-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    const auto config = Config(root, "producer", 29421U, 29422U);
    auto producer = RegtestNode::Create(config);
    ASSERT_NE(producer, nullptr);
    const auto mined = producer->MineBlock();
    ASSERT_EQ(mined.error, RegtestNodeError::kNone);
    ASSERT_TRUE(mined.block_hash.has_value());
    const auto journal = config.data_directory / "blocks.dat";
    const auto wallet = config.data_directory / "wallet.dat";
    auto read = [](const std::filesystem::path& path) {
        std::ifstream stream{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    };
    const auto journal_before = read(journal);
    const auto wallet_before = read(wallet);
    ASSERT_FALSE(journal_before.empty());
    ASSERT_FALSE(wallet_before.empty());
    ASSERT_TRUE(std::filesystem::create_directory(root / "store"));
    nova::storage::GenerationError error{};
    auto store =
        nova::storage::GenerationStore::Open(root / "store", producer->network_params(), error);
    ASSERT_NE(store, nullptr);
    const auto source = std::make_shared<nova::chain::SystemValidationTimeSource>();
    store->FailNextForTesting(nova::storage::GenerationFailurePoint::kObservation);
    const auto unavailable = nova::node::PrepareLegacyMigration(
        *store, nova::consensus::NetworkId::kRegtest, source, kStartTime, config.wallet_passphrase,
        journal, wallet, root / "unavailable-validation");
    EXPECT_EQ(unavailable.error, nova::node::MigrationError::kUnavailable);
    EXPECT_FALSE(std::filesystem::exists(root / "unavailable-validation"));
    const auto missing_pass = nova::node::PrepareLegacyMigration(
        *store, nova::consensus::NetworkId::kRegtest, source, kStartTime, {}, journal, wallet,
        root / "missing-pass-validation");
    EXPECT_EQ(missing_pass.error, nova::node::MigrationError::kValidation);
    EXPECT_EQ(missing_pass.validation_error, nova::node::GenerationRecoveryError::kWallet);
    EXPECT_EQ(store->Observe().state, nova::storage::GenerationState::kEmpty);
    auto staged = nova::node::PrepareLegacyMigration(*store, nova::consensus::NetworkId::kRegtest,
                                                     source, kStartTime, config.wallet_passphrase,
                                                     journal, wallet, root / "validation-one");
    ASSERT_EQ(staged.error, nova::node::MigrationError::kNone);
    ASSERT_NE(staged.prepared, nullptr);
    EXPECT_EQ(store->Observe().state, nova::storage::GenerationState::kEmpty);
    EXPECT_EQ(read(journal), journal_before);
    EXPECT_EQ(read(wallet), wallet_before);
    const std::vector<std::uint8_t> wrong_pass{'w', 'r', 'o', 'n', 'g'};
    const auto wrong = nova::node::CommitLegacyMigration(*staged.prepared, *store,
                                                         nova::consensus::NetworkId::kRegtest,
                                                         source, kStartTime, wrong_pass);
    EXPECT_EQ(wrong.state, nova::storage::GenerationCommitState::kNotCommitted);
    EXPECT_EQ(store->Observe().state, nova::storage::GenerationState::kEmpty);
    store->FailNextForTesting(nova::storage::GenerationFailurePoint::kBeforeRootReplace);
    auto rejected = nova::node::CommitLegacyMigration(*staged.prepared, *store,
                                                      nova::consensus::NetworkId::kRegtest, source,
                                                      kStartTime, config.wallet_passphrase);
    EXPECT_EQ(rejected.state, nova::storage::GenerationCommitState::kNotCommitted);
    EXPECT_EQ(store->Observe().state, nova::storage::GenerationState::kEmpty);
    auto retry = nova::node::PrepareLegacyMigration(*store, nova::consensus::NetworkId::kRegtest,
                                                    source, kStartTime, config.wallet_passphrase,
                                                    journal, wallet, root / "validation-two");
    ASSERT_EQ(retry.error, nova::node::MigrationError::kNone);
    ASSERT_NE(retry.prepared, nullptr);
    store->FailNextForTesting(nova::storage::GenerationFailurePoint::kAfterRootReplace);
    auto committed = nova::node::CommitLegacyMigration(*retry.prepared, *store,
                                                       nova::consensus::NetworkId::kRegtest, source,
                                                       kStartTime, config.wallet_passphrase);
    ASSERT_EQ(committed.state, nova::storage::GenerationCommitState::kCommitted);
    ASSERT_NE(committed.recovered, nullptr);
    EXPECT_EQ(committed.recovered->chain().active_height(), 1U);
    EXPECT_EQ(committed.recovered->chain().active_tip(), *mined.block_hash);
    EXPECT_EQ(committed.recovered->wallet().ConfirmedBalance(),
              producer->wallet().ConfirmedBalance());
    EXPECT_EQ(read(journal), journal_before);
    EXPECT_EQ(read(wallet), wallet_before);
    const auto empty_journal = root / "empty-journal.dat";
    {
        std::ofstream file{empty_journal, std::ios::binary};
        ASSERT_TRUE(file.good());
    }
    ASSERT_TRUE(std::filesystem::create_directory(root / "invalid-store"));
    auto invalid_store = nova::storage::GenerationStore::Open(root / "invalid-store",
                                                              producer->network_params(), error);
    ASSERT_NE(invalid_store, nullptr);
    auto invalid = nova::node::PrepareLegacyMigration(
        *invalid_store, nova::consensus::NetworkId::kRegtest, source, kStartTime,
        config.wallet_passphrase, empty_journal, wallet, root / "invalid-validation");
    EXPECT_EQ(invalid.error, nova::node::MigrationError::kValidation);
    EXPECT_EQ(invalid.validation_error, nova::node::GenerationRecoveryError::kWallet);
    EXPECT_EQ(invalid_store->Observe().state, nova::storage::GenerationState::kEmpty);
    EXPECT_EQ(read(journal), journal_before);
    EXPECT_EQ(read(wallet), wallet_before);
    invalid_store.reset();
    committed.recovered.reset();
    auto session =
        nova::node::DurableSession::Open(std::move(store), nova::consensus::NetworkId::kRegtest,
                                         source, kStartTime, config.wallet_passphrase);
    ASSERT_NE(session, nullptr);
    const auto initial_view = session->View();
    ASSERT_TRUE(initial_view.has_value());
    EXPECT_EQ(initial_view->height, 1U);
    const auto wrong_address = session->GenerateAddress(root / "address-wrong-pass", wrong_pass);
    EXPECT_EQ(wrong_address.error, nova::node::DurableSessionError::kWallet);
    EXPECT_FALSE(wrong_address.address.has_value());
    EXPECT_FALSE(std::filesystem::exists(root / "address-wrong-pass"));
    {
        nova::wallet::detail::ScopedSensitiveFailureForTesting failure{
            nova::wallet::detail::SensitiveFailureStage::kFileStaging};
        const auto failed_address =
            session->GenerateAddress(root / "address-write-failed", config.wallet_passphrase);
        EXPECT_EQ(failed_address.error, nova::node::DurableSessionError::kWallet);
        EXPECT_FALSE(failed_address.address.has_value());
    }
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->tip, initial_view->tip);
    session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterRootReplace);
    const auto new_address =
        session->GenerateAddress(root / "address-success", config.wallet_passphrase);
    ASSERT_EQ(new_address.error, nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(new_address.address.has_value());
    EXPECT_EQ(new_address.address->index, 1U);
    EXPECT_EQ(session->View()->tip, initial_view->tip);
    const auto next = producer->MineBlock();
    ASSERT_EQ(next.error, RegtestNodeError::kNone);
    ASSERT_TRUE(next.block_hash.has_value());
    const auto next_index = producer->chain_state().GetBlockIndex(*next.block_hash);
    ASSERT_TRUE(next_index.has_value());
    ASSERT_TRUE(next_index->block.has_value());
    const auto bad =
        session->ApplyBlock(*next_index->block, {}, root / "session-bad-pass", wrong_pass);
    EXPECT_EQ(bad.error, nova::node::DurableSessionError::kWallet);
    EXPECT_FALSE(std::filesystem::exists(root / "session-bad-pass"));
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->tip, initial_view->tip);
    {
        nova::wallet::detail::ScopedSensitiveFailureForTesting failure{
            nova::wallet::detail::SensitiveFailureStage::kFileStaging};
        const auto wallet_failure = session->ApplyBlock(
            *next_index->block, {}, root / "session-wallet-failed", config.wallet_passphrase);
        EXPECT_EQ(wallet_failure.error, nova::node::DurableSessionError::kWallet);
    }
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->tip, initial_view->tip);
    EXPECT_EQ(session->View()->height, initial_view->height);
    EXPECT_EQ(session->View()->confirmed_balance, initial_view->confirmed_balance);
    session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterWallet);
    const auto failed = session->ApplyBlock(*next_index->block, {}, root / "session-failed",
                                            config.wallet_passphrase);
    EXPECT_EQ(failed.error, nova::node::DurableSessionError::kStorage);
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->tip, initial_view->tip);
    EXPECT_EQ(session->View()->confirmed_balance, initial_view->confirmed_balance);
    const auto accepted = session->ApplyBlock(*next_index->block, {}, root / "session-accepted",
                                              config.wallet_passphrase);
    ASSERT_EQ(accepted.error, nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->height, 2U);
    EXPECT_EQ(session->View()->tip, *next.block_hash);
    const auto third = producer->MineBlock();
    ASSERT_EQ(third.error, RegtestNodeError::kNone);
    ASSERT_TRUE(third.block_hash.has_value());
    const auto third_index = producer->chain_state().GetBlockIndex(*third.block_hash);
    ASSERT_TRUE(third_index.has_value());
    ASSERT_TRUE(third_index->block.has_value());
    session->FailAdvanceForTesting();
    const auto uncertain = session->ApplyBlock(*third_index->block, {}, root / "session-uncertain",
                                               config.wallet_passphrase);
    EXPECT_EQ(uncertain.error, nova::node::DurableSessionError::kUnavailable);
    EXPECT_FALSE(session->View().has_value());
    EXPECT_EQ(session
                  ->ApplyBlock(*third_index->block, {}, root / "session-after-uncertain",
                               config.wallet_passphrase)
                  .error,
              nova::node::DurableSessionError::kUnavailable);
    EXPECT_FALSE(std::filesystem::exists(root / "session-after-uncertain"));
    session.reset();
    auto reopened =
        nova::storage::GenerationStore::Open(root / "store", producer->network_params(), error);
    ASSERT_NE(reopened, nullptr);
    auto replayed = nova::node::RecoverGeneration(*reopened, nova::consensus::NetworkId::kRegtest,
                                                  source, kStartTime, config.wallet_passphrase);
    ASSERT_EQ(replayed.error, nova::node::GenerationRecoveryError::kNone);
    ASSERT_NE(replayed.recovered, nullptr);
    EXPECT_EQ(replayed.recovered->chain().active_height(), 3U);
    EXPECT_EQ(replayed.recovered->chain().active_tip(), *third.block_hash);
    const auto replayed_addresses = replayed.recovered->wallet().ListReceivingAddresses();
    ASSERT_EQ(replayed_addresses.size(), 2U);
    EXPECT_EQ(replayed_addresses[1].public_key_hash, new_address.address->public_key_hash);
    replayed.recovered.reset();
    reopened.reset();
    producer.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
}

TEST(RegtestDurableSession, MiningStagesNewKeyAndBlockAsOneGeneration)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("novacoin-session-mining-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    ASSERT_TRUE(std::filesystem::create_directory(root / "store"));
    const auto& network = nova::consensus::RegtestNetworkParams();
    const std::vector<std::uint8_t> passphrase{'m', 'i', 'n', 'e'};
    ASSERT_TRUE(std::filesystem::create_directory(root / "fresh-store"));
    nova::storage::GenerationError fresh_error{};
    auto fresh_store =
        nova::storage::GenerationStore::Open(root / "fresh-store", network, fresh_error);
    ASSERT_NE(fresh_store, nullptr);
    const auto fresh_time = std::make_shared<nova::chain::SystemValidationTimeSource>();
    fresh_store->FailNextForTesting(nova::storage::GenerationFailurePoint::kAfterMempool);
    EXPECT_EQ(nova::node::DurableSession::CreateFresh(
                  std::move(fresh_store), nova::consensus::NetworkId::kRegtest, fresh_time,
                  kStartTime, passphrase, root / "fresh-failed"),
              nullptr);
    fresh_store = nova::storage::GenerationStore::Open(root / "fresh-store", network, fresh_error);
    ASSERT_NE(fresh_store, nullptr);
    EXPECT_EQ(fresh_store->Observe().state, nova::storage::GenerationState::kEmpty);
    auto fresh = nova::node::DurableSession::CreateFresh(
        std::move(fresh_store), nova::consensus::NetworkId::kRegtest, fresh_time, kStartTime,
        passphrase, root / "fresh-input");
    ASSERT_NE(fresh, nullptr);
    ASSERT_TRUE(fresh->View().has_value());
    EXPECT_EQ(fresh->View()->height, 0U);
    EXPECT_EQ(fresh->View()->mempool_size, 0U);
    const auto initial_blocks = fresh->ActiveBlocks();
    ASSERT_TRUE(initial_blocks.has_value());
    ASSERT_EQ(initial_blocks->size(), 1U);
    EXPECT_EQ(initial_blocks->back().hash, fresh->View()->tip);
    EXPECT_EQ(fresh->View()->chain_work, initial_blocks->back().chain_work);
    EXPECT_EQ(fresh->View()->mempool_bytes, 0U);
    fresh->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_FALSE(fresh->ActiveBlocks().has_value());
    const auto sync_start = fresh->StartHeaderSync();
    ASSERT_EQ(sync_start.error, nova::node::SyncError::kNone);
    ASSERT_EQ(sync_start.requests.size(), 1U);
    const auto* locator =
        std::get_if<nova::net::GetHeadersMessage>(&sync_start.requests[0].message);
    ASSERT_NE(locator, nullptr);
    ASSERT_EQ(locator->locator_hashes.size(), 1U);
    EXPECT_EQ(locator->locator_hashes.front(), fresh->View()->tip);
    EXPECT_EQ(
        fresh->HandleSyncHeaders({nova::net::Command::kHeaders, nova::net::HeadersMessage{}}).error,
        nova::node::SyncError::kNone);
    EXPECT_EQ(fresh->HandleSyncHeaders({nova::net::Command::kBlock, network.genesis_block}).error,
              nova::node::SyncError::kUnexpectedMessage);
    EXPECT_EQ(fresh->View()->height, 0U);
    fresh->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_EQ(fresh->StartHeaderSync().error, nova::node::SyncError::kChainStateUnavailable);
    const auto missing = fresh->FindBlock(nova::crypto::Hash256{});
    EXPECT_TRUE(missing.available);
    EXPECT_FALSE(missing.block.has_value());
    fresh->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_FALSE(fresh->FindBlock(nova::crypto::Hash256{}).available);
    EXPECT_TRUE(fresh->View().has_value());
    fresh.reset();
    fresh_store = nova::storage::GenerationStore::Open(root / "fresh-store", network, fresh_error);
    ASSERT_NE(fresh_store, nullptr);
    EXPECT_EQ(nova::node::DurableSession::CreateFresh(
                  std::move(fresh_store), nova::consensus::NetworkId::kRegtest, fresh_time,
                  kStartTime, passphrase, root / "fresh-again"),
              nullptr);
    EXPECT_FALSE(std::filesystem::exists(root / "fresh-again"));
    const auto journal = root / "empty-journal.dat";
    {
        std::ofstream file{journal, std::ios::binary};
        ASSERT_TRUE(file.good());
    }
    const nova::wallet::WalletParams params{network.block_limits.transaction_limits, 1, 1U, true};
    auto empty_wallet = nova::wallet::Wallet::Create(params);
    ASSERT_NE(empty_wallet, nullptr);
    ASSERT_TRUE(empty_wallet->SaveEncrypted(root / "empty-wallet.dat", passphrase));
    empty_wallet.reset();
    nova::storage::GenerationError error{};
    auto store = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(store, nullptr);
    auto base = store->Prepare(journal, root / "empty-wallet.dat", error);
    ASSERT_NE(base, nullptr);
    ASSERT_EQ(store->Commit(*base).state, nova::storage::GenerationCommitState::kCommitted);
    const auto time_source = std::make_shared<nova::chain::SystemValidationTimeSource>();
    auto session =
        nova::node::DurableSession::Open(std::move(store), nova::consensus::NetworkId::kRegtest,
                                         time_source, kStartTime, passphrase);
    ASSERT_NE(session, nullptr);
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->height, 0U);
    EXPECT_EQ(session->View()->receiving_address_count, 0U);
    const std::vector<std::uint8_t> wrong{'w', 'r', 'o', 'n', 'g'};
    EXPECT_EQ(session->MineBlock({}, root / "wrong", wrong).error,
              nova::node::DurableSessionError::kWallet);
    EXPECT_FALSE(std::filesystem::exists(root / "wrong"));
    EXPECT_EQ(session->View()->height, 0U);
    EXPECT_EQ(session->View()->receiving_address_count, 0U);
    {
        nova::wallet::detail::ScopedSensitiveFailureForTesting failure{
            nova::wallet::detail::SensitiveFailureStage::kFileStaging};
        EXPECT_EQ(session->MineBlock({}, root / "failed", passphrase).error,
                  nova::node::DurableSessionError::kWallet);
    }
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->height, 0U);
    EXPECT_EQ(session->View()->receiving_address_count, 0U);
    const auto mined = session->MineBlock({}, root / "success", passphrase);
    ASSERT_EQ(mined.error, nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(mined.chain_result.block_hash.has_value());
    ASSERT_TRUE(session->View().has_value());
    EXPECT_EQ(session->View()->height, 1U);
    EXPECT_EQ(session->View()->receiving_address_count, 1U);
    EXPECT_EQ(session->View()->tip, *mined.chain_result.block_hash);
    nova::net::GetHeadersMessage header_request{
        network.protocol_version, {network.genesis_hash}, *mined.chain_result.block_hash};
    const auto served_headers = session->HeadersForLocator(header_request);
    ASSERT_TRUE(served_headers.has_value());
    ASSERT_EQ(served_headers->headers.size(), 1U);
    EXPECT_EQ(nova::primitives::ComputeBlockHash(served_headers->headers.front()),
              mined.chain_result.block_hash);
    header_request.locator_hashes.assign(101U, network.genesis_hash);
    EXPECT_TRUE(session->HeadersForLocator(header_request).has_value());
    header_request.locator_hashes.push_back(network.genesis_hash);
    EXPECT_FALSE(session->HeadersForLocator(header_request).has_value());
    header_request.locator_hashes = {*mined.chain_result.block_hash};
    const auto empty_headers = session->HeadersForLocator(header_request);
    ASSERT_TRUE(empty_headers.has_value());
    EXPECT_TRUE(empty_headers->headers.empty());
    session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_FALSE(session->HeadersForLocator(header_request).has_value());
    const std::array<nova::wallet::Recipient, 1U> build_recipients{{{nova::crypto::Hash160{}, 1}}};
    const auto locally_built = session->BuildTransaction(build_recipients);
    ASSERT_TRUE(locally_built.has_value());
    ASSERT_TRUE(locally_built->built.has_value());
    const auto signed_transaction = session->SignTransaction(locally_built->built->transaction);
    ASSERT_TRUE(signed_transaction.has_value());
    EXPECT_TRUE(signed_transaction->transaction.has_value());
    EXPECT_EQ(session->VerifyTransaction(locally_built->built->transaction), true);
    const auto malformed_sign = session->SignTransaction(nova::primitives::Transaction{});
    ASSERT_TRUE(malformed_sign.has_value());
    EXPECT_FALSE(malformed_sign->transaction.has_value());
    EXPECT_EQ(session->VerifyTransaction(nova::primitives::Transaction{}), false);
    EXPECT_EQ(session->View()->mempool_size, 0U);
    session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_FALSE(session->BuildTransaction(build_recipients).has_value());
    session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_FALSE(session->SignTransaction(locally_built->built->transaction).has_value());
    session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_FALSE(session->VerifyTransaction(locally_built->built->transaction).has_value());
    const auto found = session->FindBlock(*mined.chain_result.block_hash);
    EXPECT_TRUE(found.available);
    ASSERT_TRUE(found.block.has_value());
    EXPECT_EQ(found.block->height, 1U);
    ASSERT_TRUE(found.block->block.has_value());
    EXPECT_EQ(session->HeaderBodyState(*mined.chain_result.block_hash),
              nova::node::DurableBodyState::kValidated);
    EXPECT_EQ(session->HeaderBodyState(nova::crypto::Hash256{}),
              nova::node::DurableBodyState::kStale);
    auto next_header = found.block->block->header;
    next_header.previous_block_id = *mined.chain_result.block_hash;
    ++next_header.time;
    const auto next_mined = nova::consensus::MineRegtestBlock(next_header, 4'096U);
    ASSERT_TRUE(next_mined.has_value());
    nova::net::HeadersMessage pending_headers;
    pending_headers.headers.push_back(next_mined->header);
    EXPECT_EQ(session->HandleSyncHeaders({nova::net::Command::kHeaders, pending_headers}).error,
              nova::node::SyncError::kNone);
    EXPECT_EQ(session->HeaderBodyState(next_mined->block_hash),
              nova::node::DurableBodyState::kPending);
    session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_FALSE(session->HeaderBodyState(next_mined->block_hash).has_value());
    const auto coinbase_id =
        found.block->block->transactions.front().TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(coinbase_id.has_value());
    const auto coin = session->ReadCoin({*coinbase_id, 0U});
    EXPECT_EQ(coin.error, nova::chain::CoinReadError::kNone);
    EXPECT_TRUE(coin.coin.has_value());
    const auto absent_coin = session->ReadCoin({nova::crypto::Hash256{}, 0U});
    EXPECT_EQ(absent_coin.error, nova::chain::CoinReadError::kNone);
    EXPECT_FALSE(absent_coin.coin.has_value());
    session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_EQ(session->ReadCoin({*coinbase_id, 0U}).error,
              nova::chain::CoinReadError::kUnavailable);
    session.reset();
    auto reopened = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(reopened, nullptr);
    auto recovered = nova::node::RecoverGeneration(*reopened, nova::consensus::NetworkId::kRegtest,
                                                   time_source, kStartTime, passphrase);
    ASSERT_EQ(recovered.error, nova::node::GenerationRecoveryError::kNone);
    ASSERT_NE(recovered.recovered, nullptr);
    EXPECT_EQ(recovered.recovered->chain().active_tip(), *mined.chain_result.block_hash);
    EXPECT_EQ(recovered.recovered->wallet().ReceivingAddressCount(), 1U);
    EXPECT_GT(recovered.recovered->wallet().ConfirmedBalance(), 0);
    const nova::chain::MempoolParams pool_params{nova::chain::NetworkValidationParams(network),
                                                 1000U, 10000000U, 0};
    const auto image = nova::chain::EncodeMempoolImage({}, pool_params);
    ASSERT_TRUE(image.has_value());
    const std::string image_text(image->begin(), image->end());
    std::istringstream image_stream{image_text};
    const auto active = recovered.recovered->chain().GetActiveBlockIndexes();
    ASSERT_TRUE(active.has_value());
    auto view = recovered.recovered->utxos().BeginBatch();
    ASSERT_TRUE(view.has_value());
    auto checked = nova::node::ValidateRecoveredMempool(
        image_stream, pool_params, {2U, recovered.recovered->next_time()}, *view,
        recovered.recovered->wallet(), *active, true);
    ASSERT_NE(checked, nullptr);
    EXPECT_EQ(checked->size(), 0U);
    std::istringstream bad_image{image_text + "x"};
    EXPECT_EQ(nova::node::ValidateRecoveredMempool(bad_image, pool_params,
                                                   {2U, recovered.recovered->next_time()}, *view,
                                                   recovered.recovered->wallet(), *active, true),
              nullptr);
    view.reset();
    recovered.recovered.reset();
    const auto legacy = reopened->Observe();
    ASSERT_EQ(legacy.state, nova::storage::GenerationState::kPresent);
    auto pending_wallet =
        nova::wallet::Wallet::LoadEncrypted(params, legacy.snapshot.wallet, passphrase);
    ASSERT_NE(pending_wallet, nullptr);
    const auto addresses = pending_wallet->ListReceivingAddresses();
    ASSERT_FALSE(addresses.empty());
    const std::vector<nova::wallet::Recipient> recipients{{addresses.front().public_key_hash, 1}};
    const auto transaction = pending_wallet->CreateTransaction(recipients);
    ASSERT_TRUE(transaction.built.has_value());
    ASSERT_TRUE(std::filesystem::create_directory(root / "admission-store"));
    auto admission_store =
        nova::storage::GenerationStore::Open(root / "admission-store", network, error);
    ASSERT_NE(admission_store, nullptr);
    auto admission_base =
        admission_store->Prepare(legacy.snapshot.journal, legacy.snapshot.wallet, error);
    ASSERT_NE(admission_base, nullptr);
    ASSERT_EQ(admission_store->Commit(*admission_base).state,
              nova::storage::GenerationCommitState::kCommitted);
    auto admission_session = nova::node::DurableSession::Open(std::move(admission_store),
                                                              nova::consensus::NetworkId::kRegtest,
                                                              time_source, kStartTime, passphrase);
    ASSERT_NE(admission_session, nullptr);
    admission_session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterMempool);
    EXPECT_EQ(admission_session->AdmitTransaction(transaction.built->transaction,
                                                  root / "admit-fail", passphrase),
              nova::node::DurableSessionError::kStorage);
    ASSERT_TRUE(admission_session->View().has_value());
    EXPECT_EQ(admission_session->View()->mempool_size, 0U);
    auto invalid_transaction = transaction.built->transaction;
    invalid_transaction.outputs.front().value += 1;
    EXPECT_EQ(admission_session->AdmitTransaction(invalid_transaction, root / "admit-invalid",
                                                  passphrase),
              nova::node::DurableSessionError::kMempool);
    EXPECT_FALSE(std::filesystem::exists(root / "admit-invalid"));
    EXPECT_EQ(admission_session->View()->mempool_size, 0U);
    const auto no_recipients =
        admission_session->BuildAndAdmit({}, root / "payment-empty", passphrase);
    EXPECT_EQ(no_recipients.error, nova::node::DurableSessionError::kWallet);
    EXPECT_FALSE(no_recipients.transaction.has_value());
    EXPECT_FALSE(std::filesystem::exists(root / "payment-empty"));
    admission_session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    const auto unavailable_payment =
        admission_session->BuildAndAdmit(recipients, root / "payment-unavailable", passphrase);
    EXPECT_EQ(unavailable_payment.error, nova::node::DurableSessionError::kUnavailable);
    EXPECT_FALSE(unavailable_payment.transaction.has_value());
    EXPECT_FALSE(std::filesystem::exists(root / "payment-unavailable"));
    EXPECT_EQ(admission_session->View()->mempool_size, 0U);
    admission_session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterMempool);
    const auto failed_payment =
        admission_session->BuildAndAdmit(recipients, root / "payment-fail", passphrase);
    EXPECT_EQ(failed_payment.error, nova::node::DurableSessionError::kStorage);
    EXPECT_FALSE(failed_payment.transaction.has_value());
    EXPECT_EQ(admission_session->View()->mempool_size, 0U);
    admission_session->FailStoreForTesting(
        nova::storage::GenerationFailurePoint::kAfterRootReplace);
    const auto payment =
        admission_session->BuildAndAdmit(recipients, root / "admit-ok", passphrase);
    ASSERT_EQ(payment.error, nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(payment.transaction.has_value());
    EXPECT_EQ(admission_session->View()->mempool_size, 1U);
    const auto wallet_view = admission_session->WalletView();
    ASSERT_TRUE(wallet_view.has_value());
    EXPECT_EQ(wallet_view->confirmed, 0);
    EXPECT_TRUE(std::any_of(wallet_view->coins.begin(), wallet_view->coins.end(),
                            [](const auto& coin) { return coin.pending_spend; }));
    admission_session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_FALSE(admission_session->WalletView().has_value());
    const auto admitted_id = payment.transaction->TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(admitted_id.has_value());
    const auto entry = admission_session->FindMempoolEntry(*admitted_id);
    EXPECT_TRUE(entry.available);
    ASSERT_TRUE(entry.entry.has_value());
    EXPECT_EQ(entry.entry->transaction_id, *admitted_id);
    const auto missing_entry = admission_session->FindMempoolEntry(nova::crypto::Hash256{});
    EXPECT_TRUE(missing_entry.available);
    EXPECT_FALSE(missing_entry.entry.has_value());
    admission_session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kObservation);
    EXPECT_FALSE(admission_session->FindMempoolEntry(*admitted_id).available);
    EXPECT_EQ(admission_session->AdmitTransaction(*payment.transaction, root / "admit-duplicate",
                                                  passphrase),
              nova::node::DurableSessionError::kMempool);
    EXPECT_EQ(admission_session->View()->mempool_size, 1U);
    const auto before_pending_address = admission_session->View();
    ASSERT_TRUE(before_pending_address.has_value());
    admission_session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterMempool);
    const auto failed_pending_address =
        admission_session->GenerateAddress(root / "pending-address-fail", passphrase);
    EXPECT_EQ(failed_pending_address.error, nova::node::DurableSessionError::kStorage);
    EXPECT_FALSE(failed_pending_address.address.has_value());
    ASSERT_TRUE(admission_session->View().has_value());
    EXPECT_EQ(admission_session->View()->receiving_address_count,
              before_pending_address->receiving_address_count);
    EXPECT_EQ(admission_session->View()->mempool_size, 1U);
    const auto pending_address =
        admission_session->GenerateAddress(root / "pending-address-ok", passphrase);
    ASSERT_EQ(pending_address.error, nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(pending_address.address.has_value());
    ASSERT_TRUE(admission_session->View().has_value());
    EXPECT_EQ(admission_session->View()->receiving_address_count,
              before_pending_address->receiving_address_count + 1U);
    EXPECT_EQ(admission_session->View()->mempool_size, 1U);
    const auto pending_address_wallet = admission_session->WalletView();
    ASSERT_TRUE(pending_address_wallet.has_value());
    EXPECT_EQ(pending_address_wallet->confirmed, wallet_view->confirmed);
    EXPECT_EQ(pending_address_wallet->unconfirmed, wallet_view->unconfirmed);
    EXPECT_TRUE(std::any_of(pending_address_wallet->coins.begin(),
                            pending_address_wallet->coins.end(),
                            [](const auto& coin) { return coin.pending_spend; }));
    admission_session.reset();
    admission_store =
        nova::storage::GenerationStore::Open(root / "admission-store", network, error);
    ASSERT_NE(admission_store, nullptr);
    auto admitted =
        nova::node::RecoverGeneration(*admission_store, nova::consensus::NetworkId::kRegtest,
                                      time_source, kStartTime, passphrase);
    ASSERT_EQ(admitted.error, nova::node::GenerationRecoveryError::kNone);
    EXPECT_EQ(admitted.mempool->size(), 1U);
    EXPECT_EQ(admitted.recovered->wallet().ReceivingAddressCount(),
              before_pending_address->receiving_address_count + 1U);
    EXPECT_EQ(admitted.recovered->wallet().ConfirmedBalance(), wallet_view->confirmed);
    EXPECT_EQ(admitted.recovered->wallet().UnconfirmedBalance(), wallet_view->unconfirmed);
    admitted.recovered.reset();
    admission_store.reset();
    ASSERT_TRUE(pending_wallet->ObserveTransaction(transaction.built->transaction, 0U, false));
    ASSERT_TRUE(pending_wallet->SaveEncrypted(root / "pending-wallet.dat", passphrase));
    auto inconsistent =
        reopened->Prepare(legacy.snapshot.journal, root / "pending-wallet.dat", error);
    ASSERT_NE(inconsistent, nullptr);
    ASSERT_EQ(reopened->Commit(*inconsistent).state,
              nova::storage::GenerationCommitState::kCommitted);
    EXPECT_EQ(nova::node::RecoverGeneration(*reopened, nova::consensus::NetworkId::kRegtest,
                                            time_source, kStartTime, passphrase)
                  .error,
              nova::node::GenerationRecoveryError::kWallet);
    const auto pending_image = root / "pending-mempool.dat";
    {
        std::ofstream file{pending_image, std::ios::binary};
        file << "opaque pending transaction image";
        ASSERT_TRUE(file.good());
    }
    auto pending =
        reopened->Prepare(legacy.snapshot.journal, legacy.snapshot.wallet, pending_image, error);
    ASSERT_NE(pending, nullptr);
    ASSERT_EQ(reopened->Commit(*pending).state, nova::storage::GenerationCommitState::kCommitted);
    EXPECT_EQ(nova::node::RecoverGeneration(*reopened, nova::consensus::NetworkId::kRegtest,
                                            time_source, kStartTime, passphrase)
                  .error,
              nova::node::GenerationRecoveryError::kUnavailable);
    const std::array pending_records{
        nova::chain::MempoolImageRecord{transaction.built->transaction, 42U}};
    const auto nonempty_image = nova::chain::EncodeMempoolImage(pending_records, pool_params);
    ASSERT_TRUE(nonempty_image.has_value());
    {
        std::ofstream file{pending_image, std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<const char*>(nonempty_image->data()),
                   static_cast<std::streamsize>(nonempty_image->size()));
        ASSERT_TRUE(file.good());
    }
    auto mismatch =
        reopened->Prepare(legacy.snapshot.journal, legacy.snapshot.wallet, pending_image, error);
    ASSERT_NE(mismatch, nullptr);
    ASSERT_EQ(reopened->Commit(*mismatch).state, nova::storage::GenerationCommitState::kCommitted);
    EXPECT_EQ(nova::node::RecoverGeneration(*reopened, nova::consensus::NetworkId::kRegtest,
                                            time_source, kStartTime, passphrase)
                  .error,
              nova::node::GenerationRecoveryError::kUnavailable);
    auto pending_generation = reopened->Prepare(legacy.snapshot.journal,
                                                root / "pending-wallet.dat", pending_image, error);
    ASSERT_NE(pending_generation, nullptr);
    ASSERT_EQ(reopened->Commit(*pending_generation).state,
              nova::storage::GenerationCommitState::kCommitted);
    auto nonempty = nova::node::RecoverGeneration(*reopened, nova::consensus::NetworkId::kRegtest,
                                                  time_source, kStartTime, passphrase);
    ASSERT_EQ(nonempty.error, nova::node::GenerationRecoveryError::kNone);
    ASSERT_NE(nonempty.mempool, nullptr);
    EXPECT_EQ(nonempty.mempool->size(), 1U);
    EXPECT_EQ(nonempty.recovered->wallet().ConfirmedBalance(), 0);
    nonempty.recovered.reset();
    auto pending_session =
        nova::node::DurableSession::Open(std::move(reopened), nova::consensus::NetworkId::kRegtest,
                                         time_source, kStartTime, passphrase);
    ASSERT_NE(pending_session, nullptr);
    ASSERT_TRUE(pending_session->View().has_value());
    EXPECT_EQ(pending_session->View()->mempool_size, 1U);
    const auto pending_before_failure = pending_session->View();
    pending_session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterMempool);
    EXPECT_EQ(pending_session->MineBlock({}, root / "pending-failed", passphrase).error,
              nova::node::DurableSessionError::kStorage);
    ASSERT_TRUE(pending_session->View().has_value());
    EXPECT_EQ(pending_session->View()->tip, pending_before_failure->tip);
    EXPECT_EQ(pending_session->View()->mempool_size, 1U);
    pending_session.reset();
    reopened = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(reopened, nullptr);
    pending_session =
        nova::node::DurableSession::Open(std::move(reopened), nova::consensus::NetworkId::kRegtest,
                                         time_source, kStartTime, passphrase);
    ASSERT_NE(pending_session, nullptr);
    ASSERT_TRUE(pending_session->View().has_value());
    EXPECT_EQ(pending_session->View()->tip, pending_before_failure->tip);
    EXPECT_EQ(pending_session->View()->mempool_size, 1U);
    EXPECT_EQ(pending_session->View()->confirmed_balance,
              pending_before_failure->confirmed_balance);
    const auto fork_anchor = pending_session->FindBlock(pending_before_failure->tip);
    ASSERT_TRUE(fork_anchor.available);
    ASSERT_TRUE(fork_anchor.block.has_value());
    ASSERT_TRUE(fork_anchor.block->block.has_value());
    auto fork_producer = RegtestNode::Create(Config(root, "fork-producer", 29341U, 29342U));
    ASSERT_NE(fork_producer, nullptr);
    ASSERT_EQ(fork_producer->ReceiveBlock(*fork_anchor.block->block), RegtestNodeError::kNone);
    const auto fork_first = fork_producer->MineBlock();
    ASSERT_EQ(fork_first.error, RegtestNodeError::kNone);
    ASSERT_TRUE(fork_first.block_hash.has_value());
    const auto fork_first_index =
        fork_producer->chain_state().GetBlockIndex(*fork_first.block_hash);
    ASSERT_TRUE(fork_first_index.has_value());
    ASSERT_TRUE(fork_first_index->block.has_value());
    const auto fork_second = fork_producer->MineBlock();
    ASSERT_EQ(fork_second.error, RegtestNodeError::kNone);
    ASSERT_TRUE(fork_second.block_hash.has_value());
    const auto fork_second_index =
        fork_producer->chain_state().GetBlockIndex(*fork_second.block_hash);
    ASSERT_TRUE(fork_second_index.has_value());
    ASSERT_TRUE(fork_second_index->block.has_value());
    pending_session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterRootReplace);
    const auto confirmed_pending =
        pending_session->MineBlock({}, root / "pending-mine", passphrase);
    ASSERT_EQ(confirmed_pending.error, nova::node::DurableSessionError::kNone);
    EXPECT_EQ(pending_session->View()->mempool_size, 0U);
    pending_session.reset();
    reopened = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(reopened, nullptr);
    auto after_pending = nova::node::RecoverGeneration(
        *reopened, nova::consensus::NetworkId::kRegtest, time_source, kStartTime, passphrase);
    ASSERT_EQ(after_pending.error, nova::node::GenerationRecoveryError::kNone);
    EXPECT_EQ(after_pending.mempool->size(), 0U);
    EXPECT_EQ(after_pending.recovered->chain().active_height(), 2U);
    after_pending.recovered.reset();
    pending_session =
        nova::node::DurableSession::Open(std::move(reopened), nova::consensus::NetworkId::kRegtest,
                                         time_source, kStartTime, passphrase);
    ASSERT_NE(pending_session, nullptr);
    ASSERT_EQ(
        pending_session->ApplyBlock(*fork_first_index->block, {}, root / "fork-first", passphrase)
            .error,
        nova::node::DurableSessionError::kNone);
    const auto before_fork_failure = pending_session->View();
    const auto before_fork_wallet = pending_session->WalletView();
    ASSERT_TRUE(before_fork_failure.has_value());
    ASSERT_TRUE(before_fork_wallet.has_value());
    ASSERT_FALSE(transaction.built->transaction.inputs.empty());
    const auto source_key = nova::chain::UTXOKey::FromOutPoint(
        transaction.built->transaction.inputs.front().previous_output);
    const auto before_fork_coin = pending_session->ReadCoin(source_key);
    ASSERT_EQ(before_fork_coin.error, nova::chain::CoinReadError::kNone);
    pending_session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kBeforeRootReplace);
    EXPECT_EQ(
        pending_session
            ->ApplyBlock(*fork_second_index->block, {}, root / "fork-second-failed", passphrase)
            .error,
        nova::node::DurableSessionError::kStorage);
    ASSERT_TRUE(pending_session->View().has_value());
    EXPECT_EQ(pending_session->View()->tip, before_fork_failure->tip);
    EXPECT_EQ(pending_session->View()->height, before_fork_failure->height);
    EXPECT_EQ(pending_session->View()->mempool_size, before_fork_failure->mempool_size);
    pending_session.reset();
    reopened = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(reopened, nullptr);
    pending_session =
        nova::node::DurableSession::Open(std::move(reopened), nova::consensus::NetworkId::kRegtest,
                                         time_source, kStartTime, passphrase);
    ASSERT_NE(pending_session, nullptr);
    ASSERT_TRUE(pending_session->View().has_value());
    EXPECT_EQ(pending_session->View()->tip, before_fork_failure->tip);
    EXPECT_EQ(pending_session->View()->height, before_fork_failure->height);
    EXPECT_EQ(pending_session->View()->mempool_size, before_fork_failure->mempool_size);
    const auto after_fork_failure_wallet = pending_session->WalletView();
    ASSERT_TRUE(after_fork_failure_wallet.has_value());
    EXPECT_EQ(after_fork_failure_wallet->confirmed, before_fork_wallet->confirmed);
    EXPECT_EQ(after_fork_failure_wallet->unconfirmed, before_fork_wallet->unconfirmed);
    const auto after_fork_failure_coin = pending_session->ReadCoin(source_key);
    ASSERT_EQ(after_fork_failure_coin.error, nova::chain::CoinReadError::kNone);
    EXPECT_EQ(after_fork_failure_coin.coin.has_value(), before_fork_coin.coin.has_value());
    ASSERT_EQ(
        pending_session->ApplyBlock(*fork_second_index->block, {}, root / "fork-second", passphrase)
            .error,
        nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(pending_session->View().has_value());
    EXPECT_EQ(pending_session->View()->tip, *fork_second.block_hash);
    EXPECT_EQ(pending_session->View()->mempool_size, 1U);
    const auto restored_source = pending_session->ReadCoin(source_key);
    EXPECT_EQ(restored_source.error, nova::chain::CoinReadError::kNone);
    EXPECT_TRUE(restored_source.coin.has_value());
    const auto restored_wallet = pending_session->WalletView();
    ASSERT_TRUE(restored_wallet.has_value());
    EXPECT_TRUE(std::any_of(restored_wallet->coins.begin(), restored_wallet->coins.end(),
                            [](const auto& coin) { return coin.pending_spend; }));
    const auto restored_id =
        transaction.built->transaction.TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(restored_id.has_value());
    ASSERT_TRUE(pending_session->FindMempoolEntry(*restored_id).entry.has_value());
    pending_session.reset();
    reopened = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(reopened, nullptr);
    auto after_fork =
        nova::node::DurableSession::Open(std::move(reopened), nova::consensus::NetworkId::kRegtest,
                                         time_source, kStartTime, passphrase);
    ASSERT_NE(after_fork, nullptr);
    ASSERT_TRUE(after_fork->View().has_value());
    EXPECT_EQ(after_fork->View()->tip, *fork_second.block_hash);
    EXPECT_EQ(after_fork->View()->mempool_size, 1U);
    EXPECT_TRUE(after_fork->FindMempoolEntry(*restored_id).entry.has_value());
    after_fork.reset();
    fork_producer.reset();
    reopened = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(reopened, nullptr);
    {
        std::ofstream file{pending_image, std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<const char*>(image->data()),
                   static_cast<std::streamsize>(image->size()));
        ASSERT_TRUE(file.good());
    }
    auto valid =
        reopened->Prepare(legacy.snapshot.journal, legacy.snapshot.wallet, pending_image, error);
    ASSERT_NE(valid, nullptr);
    ASSERT_EQ(reopened->Commit(*valid).state, nova::storage::GenerationCommitState::kCommitted);
    auto v2 = nova::node::RecoverGeneration(*reopened, nova::consensus::NetworkId::kRegtest,
                                            time_source, kStartTime, passphrase);
    ASSERT_EQ(v2.error, nova::node::GenerationRecoveryError::kNone);
    ASSERT_NE(v2.mempool, nullptr);
    EXPECT_EQ(v2.mempool->size(), 0U);
    v2.recovered.reset();
    auto v2_session =
        nova::node::DurableSession::Open(std::move(reopened), nova::consensus::NetworkId::kRegtest,
                                         time_source, kStartTime, passphrase);
    ASSERT_NE(v2_session, nullptr);
    const auto address_result = v2_session->GenerateAddress(root / "v2-address", passphrase);
    ASSERT_EQ(address_result.error, nova::node::DurableSessionError::kNone);
    const auto before_failure = v2_session->View();
    ASSERT_TRUE(before_failure.has_value());
    v2_session->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterMempool);
    const auto failed_v2 = v2_session->MineBlock({}, root / "v2-failed", passphrase);
    EXPECT_EQ(failed_v2.error, nova::node::DurableSessionError::kStorage);
    ASSERT_TRUE(v2_session->View().has_value());
    EXPECT_EQ(v2_session->View()->tip, before_failure->tip);
    EXPECT_EQ(v2_session->View()->height, before_failure->height);
    EXPECT_EQ(v2_session->View()->confirmed_balance, before_failure->confirmed_balance);
    EXPECT_EQ(v2_session->View()->mempool_size, before_failure->mempool_size);
    v2_session.reset();
    reopened = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(reopened, nullptr);
    v2_session =
        nova::node::DurableSession::Open(std::move(reopened), nova::consensus::NetworkId::kRegtest,
                                         time_source, kStartTime, passphrase);
    ASSERT_NE(v2_session, nullptr);
    ASSERT_TRUE(v2_session->View().has_value());
    EXPECT_EQ(v2_session->View()->tip, before_failure->tip);
    EXPECT_EQ(v2_session->View()->height, before_failure->height);
    const auto v2_mined = v2_session->MineBlock({}, root / "v2-mine", passphrase);
    ASSERT_EQ(v2_mined.error, nova::node::DurableSessionError::kNone);
    EXPECT_EQ(v2_session->View()->height, 2U);
    v2_session.reset();
    reopened = nova::storage::GenerationStore::Open(root / "store", network, error);
    ASSERT_NE(reopened, nullptr);
    auto after_address = nova::node::RecoverGeneration(
        *reopened, nova::consensus::NetworkId::kRegtest, time_source, kStartTime, passphrase);
    ASSERT_EQ(after_address.error, nova::node::GenerationRecoveryError::kNone);
    ASSERT_NE(after_address.mempool, nullptr);
    EXPECT_EQ(after_address.mempool->size(), 0U);
    EXPECT_EQ(after_address.recovered->chain().active_height(), 2U);
    after_address.recovered.reset();
    reopened.reset();
    ASSERT_EQ(std::filesystem::canonical(root).parent_path(),
              std::filesystem::canonical(std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
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

    alpha.reset();
    bravo.reset();
    charlie.reset();
    delta.reset();
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

TEST(RegtestHarness, RefusesDisabledMainnetBeforeCreatingPersistentState)
{
    const auto root = std::filesystem::temp_directory_path() / "novacoin-disabled-mainnet";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_FALSE(error);
    auto config = Config(root, "mainnet", 18644U, 18643U);
    config.network = nova::consensus::NetworkId::kMainnet;

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
