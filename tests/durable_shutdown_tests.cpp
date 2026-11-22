#include "daemon/durable_runtime.hpp"
#include "node/durable_node.hpp"
#include "node/durable_startup.hpp"
#include "wallet/address.hpp"
#include "wallet/secure_memory.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
class DurableShutdownTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path() /
                ("nova-durable-shutdown-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(root_));
        time_ = std::make_shared<nova::chain::FixedValidationTimeSource>(1'704'067'800U);
        node_ = Open();
        ASSERT_NE(node_, nullptr);
    }
    void TearDown() override
    {
        node_.reset();
        if (HasFailure()) {
            std::fprintf(stderr, "Retained shutdown fixture: %s\n", root_.string().c_str());
            return;
        }
        ASSERT_EQ(std::filesystem::canonical(root_).parent_path(),
                  std::filesystem::canonical(std::filesystem::temp_directory_path()));
        std::filesystem::remove_all(root_);
    }
    std::unique_ptr<nova::node::DurableNode> Open()
    {
        auto state = nova::node::OpenDurableNodeState(
            root_ / "node", nova::consensus::NetworkId::kRegtest, time_, time_->Now(), passphrase_);
        if (!state)
            return nullptr;
        observer_ = state->session.get();
        return nova::node::DurableNode::Create(std::move(state->session),
                                               std::move(state->workspace), passphrase_,
                                               std::move(state->directory_lease));
    }
    std::unique_ptr<nova::daemon::DurableRuntime> OpenRuntime()
    {
        node_.reset();
        auto state = nova::node::OpenDurableNodeState(
            root_ / "node", nova::consensus::NetworkId::kRegtest, time_, time_->Now(), passphrase_);
        if (!state)
            return nullptr;
        observer_ = state->session.get();
        return nova::daemon::DurableRuntime::Create(std::move(state->session),
                                                    std::move(state->workspace), passphrase_,
                                                    {"127.0.0.1",
                                                     "user",
                                                     "pass",
                                                     {8192U, 1000000U, 4096U},
                                                     nova::consensus::NetworkId::kRegtest},
                                                    std::move(state->directory_lease));
    }
    std::filesystem::path Selector() const
    {
        return root_ / "node" / "generations" / "generation.current";
    }
    static std::string Read(const std::filesystem::path& path)
    {
        std::ifstream input{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, {}};
    }
    void CheckUncertainTransactionRecovery(const bool wallet_send)
    {
        const auto address = node_->GenerateAddress();
        ASSERT_TRUE(address);
        ASSERT_TRUE(node_->Mine());
        const auto before = node_->View();
        const auto wallet_before = node_->Session().WalletView();
        ASSERT_TRUE(before && wallet_before);
        ASSERT_EQ(wallet_before->coins.size(), 1U);
        const auto coin = wallet_before->coins.front();
        const std::array<nova::wallet::Recipient, 1U> recipients{{{address->public_key_hash, 1}}};
        const auto planned = node_->Session().BuildTransaction(recipients);
        ASSERT_TRUE(planned && planned->built);
        const auto& transaction = planned->built->transaction;
        const auto limits = node_->Session().network_params().block_limits.transaction_limits;
        auto id = transaction.TxId(limits);
        ASSERT_TRUE(id);
        const auto prior_root = Read(Selector());
        observer_->FailAdvanceForTesting();
        if (wallet_send) {
            EXPECT_FALSE(node_->Send(recipients.front()));
            id = node_->UncertainPayment();
            ASSERT_TRUE(id);
        } else {
            EXPECT_EQ(node_->ReceiveTransaction(transaction),
                      nova::node::DurableSessionError::kUnavailable);
        }
        EXPECT_FALSE(node_->Ready());
        EXPECT_FALSE(node_->View());
        EXPECT_FALSE(node_->Peer().DurableAvailable());
        EXPECT_FALSE(node_->Shutdown());
        EXPECT_EQ(node_->Metrics().transactions_rejected, 0U);
        EXPECT_EQ(node_->Metrics().transactions_accepted, 0U);
        const auto committed_root = Read(Selector());
        EXPECT_NE(committed_root, prior_root);
        EXPECT_FALSE(node_->GenerateAddress());
        EXPECT_FALSE(node_->Mine());
        EXPECT_EQ(node_->ReceiveTransaction(transaction),
                  nova::node::DurableSessionError::kUnavailable);
        EXPECT_EQ(Read(Selector()), committed_root);
        std::vector<std::uint8_t> canonical;
        for (unsigned restart = 0; restart < 3U; ++restart) {
            node_.reset();
            node_ = Open();
            ASSERT_NE(node_, nullptr);
            const auto view = node_->View();
            const auto wallet = node_->Session().WalletView();
            ASSERT_TRUE(view && wallet);
            EXPECT_EQ(view->tip, before->tip);
            EXPECT_EQ(view->height, before->height);
            EXPECT_EQ(view->chain_work, before->chain_work);
            EXPECT_EQ(view->receiving_address_count, before->receiving_address_count);
            EXPECT_EQ(view->mempool_size, 1U);
            EXPECT_EQ(wallet->confirmed, 0);
            const auto pending = node_->Session().FindMempoolEntry(*id);
            ASSERT_TRUE(pending.available && pending.entry);
            const auto& actual = pending.entry->transaction;
            ASSERT_EQ(actual.inputs.size(), 1U);
            EXPECT_EQ(actual.inputs.front().previous_output.transaction_id,
                      coin.key.transaction_id);
            EXPECT_EQ(actual.inputs.front().previous_output.output_index, coin.key.output_index);
            ASSERT_EQ(actual.outputs.size(), transaction.outputs.size());
            nova::primitives::Amount total{};
            for (std::size_t i = 0; i < actual.outputs.size(); ++i) {
                EXPECT_EQ(actual.outputs[i].value, transaction.outputs[i].value);
                EXPECT_EQ(actual.outputs[i].script_pubkey, transaction.outputs[i].script_pubkey);
                ASSERT_GE(actual.outputs[i].value, 0);
                ASSERT_LE(actual.outputs[i].value, limits.max_money - total);
                total += actual.outputs[i].value;
            }
            EXPECT_EQ(wallet->unconfirmed, total);
            ASSERT_EQ(wallet->coins.size(), actual.outputs.size() + 1U);
            const auto spent =
                std::find_if(wallet->coins.begin(), wallet->coins.end(),
                             [&](const auto& value) { return value.key == coin.key; });
            ASSERT_NE(spent, wallet->coins.end());
            EXPECT_TRUE(spent->confirmed && spent->pending_spend);
            const auto utxo = node_->Session().ReadCoin(coin.key);
            ASSERT_EQ(utxo.error, nova::chain::CoinReadError::kNone);
            ASSERT_TRUE(utxo.coin);
            EXPECT_EQ(utxo.coin->output.value, coin.output.value);
            EXPECT_EQ(utxo.coin->output.script_pubkey, coin.output.script_pubkey);
            nova::primitives::BinaryWriter encoded;
            ASSERT_TRUE(actual.Serialize(encoded, limits));
            if (restart == 0U)
                canonical.assign(encoded.bytes().begin(), encoded.bytes().end());
            EXPECT_TRUE(std::equal(canonical.begin(), canonical.end(), encoded.bytes().begin(),
                                   encoded.bytes().end()));
            EXPECT_EQ(Read(Selector()), committed_root);
        }
    }
    void CheckRpcTransactionRecovery(const bool wallet_send)
    {
        const auto address = node_->GenerateAddress();
        ASSERT_TRUE(address);
        ASSERT_TRUE(node_->Mine());
        const auto before = node_->View();
        const auto wallet_before = node_->Session().WalletView();
        ASSERT_TRUE(before && wallet_before);
        ASSERT_EQ(wallet_before->coins.size(), 1U);
        const auto coin = wallet_before->coins.front();
        const auto& network = node_->Session().network_params();
        const auto limits = network.block_limits.transaction_limits;
        const auto encoded_address =
            nova::wallet::EncodeP2pkhAddress(address->public_key_hash, network);
        ASSERT_TRUE(encoded_address);
        const std::array<nova::wallet::Recipient, 1U> recipients{{{address->public_key_hash, 1}}};
        const auto planned = node_->Session().BuildTransaction(recipients);
        ASSERT_TRUE(planned && planned->built);
        const auto& transaction = planned->built->transaction;
        const auto planned_id = transaction.TxId(limits);
        ASSERT_TRUE(planned_id);
        nova::primitives::BinaryWriter serialized;
        ASSERT_TRUE(transaction.Serialize(serialized, limits));
        std::string hex;
        constexpr std::string_view digits{"0123456789abcdef"};
        for (const auto byte : serialized.bytes()) {
            hex.push_back(digits[byte >> 4U]);
            hex.push_back(digits[byte & 15U]);
        }
        const auto request =
            wallet_send
                ? std::string{R"({"jsonrpc":"2.0","id":1,"method":"sendtoaddress","params":{"amount":1,"address":")"} +
                      *encoded_address + R"("}})"
                : std::string{R"({"jsonrpc":"2.0","id":1,"method":"sendwallettransaction","params":{"hex":")"} +
                      hex + R"("}})";
        auto runtime = OpenRuntime();
        ASSERT_TRUE(runtime);
        constexpr std::string_view auth{"Basic dXNlcjpwYXNz"};
        const auto prior = Read(Selector());
        ASSERT_FALSE(prior.empty());
        observer_->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterWallet);
        const auto rejected = runtime->Rpc().HandleHttpPost({"POST", auth, request});
        EXPECT_EQ(rejected.status, 200U);
        EXPECT_NE(rejected.body.find("transaction broadcast failed"), std::string::npos);
        EXPECT_EQ(rejected.body.find("may have committed"), std::string::npos);
        const auto unchanged = runtime->View();
        const auto unchanged_wallet = observer_->WalletView();
        ASSERT_TRUE(unchanged && unchanged_wallet);
        EXPECT_EQ(unchanged->mempool_size, 0U);
        EXPECT_EQ(unchanged_wallet->confirmed, wallet_before->confirmed);
        ASSERT_EQ(unchanged_wallet->coins.size(), 1U);
        EXPECT_FALSE(unchanged_wallet->coins.front().pending_spend);
        EXPECT_EQ(Read(Selector()), prior);

        observer_->FailAdvanceForTesting();
        const auto uncertain = runtime->Rpc().HandleHttpPost({"POST", auth, request});
        EXPECT_EQ(uncertain.status, 200U);
        EXPECT_NE(uncertain.body.find("operation outcome may have committed"), std::string::npos);
        constexpr std::string_view label{"transaction id "};
        const auto position = uncertain.body.find(label);
        ASSERT_NE(position, std::string::npos);
        const auto id = nova::crypto::Hash256::FromHex(
            std::string_view{uncertain.body}.substr(position + label.size(), 64U));
        ASSERT_TRUE(id);
        if (!wallet_send)
            EXPECT_EQ(id, planned_id);
        EXPECT_FALSE(runtime->Ready());
        const auto committed = Read(Selector());
        EXPECT_NE(committed, prior);
        const auto refused = runtime->Rpc().HandleHttpPost({"POST", auth, request});
        EXPECT_NE(refused.body.find("node state unavailable"), std::string::npos);
        EXPECT_EQ(Read(Selector()), committed);
        runtime.reset();

        for (unsigned restart = 0U; restart < 2U; ++restart) {
            node_ = Open();
            ASSERT_TRUE(node_);
            const auto recovered = node_->View();
            ASSERT_TRUE(recovered);
            EXPECT_EQ(recovered->tip, before->tip);
            EXPECT_EQ(recovered->chain_work, before->chain_work);
            EXPECT_EQ(recovered->receiving_address_count, before->receiving_address_count);
            EXPECT_EQ(recovered->mempool_size, 1U);
            const auto pending = node_->Session().FindMempoolEntry(*id);
            ASSERT_TRUE(pending.available && pending.entry);
            EXPECT_EQ(pending.entry->transaction.TxId(limits), id);
            const auto wallet = node_->Session().WalletView();
            ASSERT_TRUE(wallet);
            EXPECT_EQ(wallet->confirmed, 0);
            const auto spent = std::find_if(wallet->coins.begin(), wallet->coins.end(),
                                            [&](const auto& item) { return item.key == coin.key; });
            ASSERT_NE(spent, wallet->coins.end());
            EXPECT_TRUE(spent->confirmed && spent->pending_spend);
            EXPECT_EQ(Read(Selector()), committed);
            node_.reset();
        }
    }

    std::filesystem::path root_;
    const std::vector<std::uint8_t> passphrase_{'t', 'e', 's', 't'};
    std::shared_ptr<nova::chain::FixedValidationTimeSource> time_;
    std::unique_ptr<nova::node::DurableNode> node_;
    nova::node::DurableSession* observer_{};
};

TEST_F(DurableShutdownTest, ReceivedTransactionRecoversAfterUncertainLivePublication)
{
    CheckUncertainTransactionRecovery(false);
}

TEST_F(DurableShutdownTest, WalletSendRecoversAfterUncertainLivePublication)
{
    CheckUncertainTransactionRecovery(true);
}

TEST_F(DurableShutdownTest, EmptyShutdownIsIdempotentAndStopsAllWriterIngress)
{
    const auto root = Read(Selector());
    ASSERT_TRUE(node_->Shutdown());
    EXPECT_TRUE(node_->Shutdown());
    EXPECT_FALSE(node_->Ready());
    EXPECT_FALSE(node_->View());
    EXPECT_FALSE(node_->GenerateAddress());
    EXPECT_FALSE(node_->Mine());
    EXPECT_FALSE(node_->Submit({}));
    EXPECT_FALSE(node_->Send({{}, 1}));
    EXPECT_EQ(node_->ReceiveBlock({}).error, nova::node::DurableSessionError::kUnavailable);
    EXPECT_FALSE(node_->Peer().DurableAvailable());
    EXPECT_EQ(node_->Peer().Listen({"127.0.0.1", 1U}),
              nova::net::TransportError::kInvalidParameters);
    EXPECT_FALSE(node_->Peer().Dial({"127.0.0.1", 1U}, 0U));
    EXPECT_EQ(Read(Selector()), root);
    node_.reset();
    node_ = Open();
    ASSERT_NE(node_, nullptr);
    const auto view = node_->View();
    ASSERT_TRUE(view);
    EXPECT_EQ(view->height, 0U);
    EXPECT_EQ(view->mempool_size, 0U);
    EXPECT_EQ(view->receiving_address_count, 0U);
    EXPECT_EQ(Read(Selector()), root);
}

TEST_F(DurableShutdownTest, MetricsDoNotLabelUncertainPublicationAsRejection)
{
    ASSERT_TRUE(node_->Mine());
    EXPECT_EQ(node_->Metrics().blocks_accepted, 1U);
    EXPECT_EQ(node_->Metrics().blocks_rejected, 0U);
    EXPECT_GT(node_->Metrics().last_block_arrival_time_seconds, 0U);
    const auto selector = Read(Selector());
    EXPECT_EQ(node_->ReceiveTransaction({}), nova::node::DurableSessionError::kMempool);
    EXPECT_EQ(node_->Metrics().transactions_rejected, 1U);
    EXPECT_EQ(Read(Selector()), selector);
    observer_->FailAdvanceForTesting();
    EXPECT_FALSE(node_->Mine());
    EXPECT_FALSE(node_->Ready());
    EXPECT_EQ(node_->Metrics().blocks_accepted, 1U);
    EXPECT_EQ(node_->Metrics().blocks_rejected, 0U);
    EXPECT_NE(Read(Selector()), selector);
    node_.reset();
    node_ = Open();
    ASSERT_NE(node_, nullptr);
    ASSERT_TRUE(node_->View());
    EXPECT_EQ(node_->View()->height, 2U);
    EXPECT_EQ(node_->Metrics().blocks_accepted, 0U); // Process-local, not persisted.
}

TEST_F(DurableShutdownTest, CommittedPendingWalletAndChainSurviveRepeatedCleanShutdown)
{
    const auto address = node_->GenerateAddress();
    ASSERT_TRUE(address);
    ASSERT_TRUE(node_->Mine());
    const auto payment = node_->Send({address->public_key_hash, 1});
    ASSERT_TRUE(payment);
    const auto before = node_->View();
    const auto wallet = node_->Session().WalletView();
    const auto entry = node_->Session().FindMempoolEntry(*payment);
    ASSERT_TRUE(before);
    ASSERT_TRUE(wallet);
    ASSERT_TRUE(entry.available && entry.entry);
    ASSERT_EQ(before->mempool_size, 1U);
    ASSERT_FALSE(wallet->coins.empty());
    const auto root = Read(Selector());
    for (unsigned restart = 0; restart < 3U; ++restart) {
        ASSERT_TRUE(node_->Shutdown());
        EXPECT_FALSE(node_->GenerateAddress());
        EXPECT_FALSE(node_->Mine());
        EXPECT_EQ(Read(Selector()), root);
        node_.reset();
        node_ = Open();
        ASSERT_NE(node_, nullptr);
        const auto after = node_->View();
        ASSERT_TRUE(after);
        EXPECT_EQ(after->tip, before->tip);
        EXPECT_EQ(after->height, before->height);
        EXPECT_EQ(after->chain_work, before->chain_work);
        EXPECT_EQ(after->next_time, before->next_time);
        EXPECT_EQ(after->mempool_size, before->mempool_size);
        EXPECT_EQ(after->receiving_address_count, before->receiving_address_count);
        const auto recovered = node_->Session().WalletView();
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered->confirmed, wallet->confirmed);
        EXPECT_EQ(recovered->unconfirmed, wallet->unconfirmed);
        ASSERT_EQ(recovered->coins.size(), wallet->coins.size());
        for (std::size_t index = 0; index < wallet->coins.size(); ++index) {
            const auto& old = wallet->coins[index];
            const auto& coin = recovered->coins[index];
            EXPECT_EQ(coin.key, old.key);
            EXPECT_EQ(coin.output.value, old.output.value);
            EXPECT_EQ(coin.output.script_pubkey, old.output.script_pubkey);
            EXPECT_EQ(coin.height, old.height);
            EXPECT_EQ(coin.is_coinbase, old.is_coinbase);
            EXPECT_EQ(coin.confirmed, old.confirmed);
            EXPECT_EQ(coin.pending_spend, old.pending_spend);
        }
        const auto pending = node_->Session().FindMempoolEntry(*payment);
        ASSERT_TRUE(pending.available && pending.entry);
        nova::primitives::BinaryWriter actual_bytes, expected_bytes;
        const auto& limits = node_->Session().network_params().block_limits.transaction_limits;
        ASSERT_TRUE(pending.entry->transaction.Serialize(actual_bytes, limits));
        ASSERT_TRUE(entry.entry->transaction.Serialize(expected_bytes, limits));
        EXPECT_TRUE(std::equal(actual_bytes.bytes().begin(), actual_bytes.bytes().end(),
                               expected_bytes.bytes().begin(), expected_bytes.bytes().end()));
        EXPECT_EQ(Read(Selector()), root);
    }
}

TEST_F(DurableShutdownTest, CorruptAuthorityCannotReportCleanShutdownOrRepairOnRestart)
{
    ASSERT_TRUE(node_->GenerateAddress());
    auto corrupt = Read(Selector());
    ASSERT_FALSE(corrupt.empty());
    corrupt.back() ^= 1;
    {
        std::ofstream output{Selector(), std::ios::binary | std::ios::trunc};
        output.write(corrupt.data(), static_cast<std::streamsize>(corrupt.size()));
        ASSERT_TRUE(output.good());
    }
    EXPECT_FALSE(node_->Shutdown());
    EXPECT_FALSE(node_->Shutdown());
    EXPECT_FALSE(node_->Ready());
    EXPECT_FALSE(node_->GenerateAddress());
    EXPECT_FALSE(node_->Mine());
    EXPECT_EQ(Read(Selector()), corrupt);
    node_.reset();
    EXPECT_EQ(Open(), nullptr);
    EXPECT_EQ(Read(Selector()), corrupt);
}

TEST_F(DurableShutdownTest, RejectedPreparationCanShutdownCleanlyWithoutPublishingAKey)
{
    const auto root = Read(Selector());
    observer_->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterWallet);
    EXPECT_FALSE(node_->GenerateAddress());
    EXPECT_TRUE(node_->Ready());
    EXPECT_TRUE(node_->Shutdown());
    EXPECT_EQ(Read(Selector()), root);
    node_.reset();
    node_ = Open();
    ASSERT_NE(node_, nullptr);
    const auto view = node_->View();
    ASSERT_TRUE(view);
    EXPECT_EQ(view->receiving_address_count, 0U);
    EXPECT_EQ(Read(Selector()), root);
}

TEST_F(DurableShutdownTest, FailedLivePublicationRequiresRecoveryOfCommittedKey)
{
    const auto root = Read(Selector());
    observer_->FailAdvanceForTesting();
    EXPECT_FALSE(node_->GenerateAddress());
    EXPECT_FALSE(node_->Ready());
    EXPECT_FALSE(node_->Shutdown());
    EXPECT_NE(Read(Selector()), root);
    node_.reset();
    node_ = Open();
    ASSERT_NE(node_, nullptr);
    const auto view = node_->View();
    ASSERT_TRUE(view);
    EXPECT_EQ(view->height, 0U);
    EXPECT_EQ(view->mempool_size, 0U);
    EXPECT_EQ(view->receiving_address_count, 1U);
    const auto next = node_->GenerateAddress();
    ASSERT_TRUE(next);
    EXPECT_EQ(next->index, 1U);
}

TEST_F(DurableShutdownTest, RpcAddressFailureDistinguishesRejectionFromDurableRecovery)
{
    auto runtime = OpenRuntime();
    ASSERT_TRUE(runtime);
    constexpr std::string_view auth{"Basic dXNlcjpwYXNz"};
    constexpr std::string_view request{R"({"jsonrpc":"2.0","id":1,"method":"getnewaddress"})"};
    const auto prior = Read(Selector());
    ASSERT_FALSE(prior.empty());
    observer_->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterWallet);
    const auto rejected = runtime->Rpc().HandleHttpPost({"POST", auth, request});
    EXPECT_EQ(rejected.status, 200U);
    EXPECT_NE(rejected.body.find("address generation failed"), std::string::npos);
    EXPECT_EQ(rejected.body.find("may have committed"), std::string::npos);
    const auto unchanged = runtime->View();
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->receiving_address_count, 0U);
    EXPECT_EQ(Read(Selector()), prior);

    observer_->FailAdvanceForTesting();
    const auto uncertain = runtime->Rpc().HandleHttpPost({"POST", auth, request});
    EXPECT_EQ(uncertain.status, 200U);
    EXPECT_NE(uncertain.body.find("operation outcome may have committed"), std::string::npos);
    EXPECT_FALSE(runtime->Ready());
    const auto committed = Read(Selector());
    EXPECT_NE(committed, prior);
    const auto refused = runtime->Rpc().HandleHttpPost({"POST", auth, request});
    EXPECT_NE(refused.body.find("node state unavailable"), std::string::npos);
    EXPECT_EQ(Read(Selector()), committed);

    runtime.reset();
    for (unsigned restart = 0U; restart < 2U; ++restart) {
        node_ = Open();
        ASSERT_TRUE(node_);
        const auto recovered = node_->View();
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered->receiving_address_count, 1U);
        EXPECT_EQ(Read(Selector()), committed);
        node_.reset();
    }
}

TEST_F(DurableShutdownTest, RpcMiningRecoversBlockAfterUncertainPublication)
{
    auto runtime = OpenRuntime();
    ASSERT_TRUE(runtime);
    constexpr std::string_view auth{"Basic dXNlcjpwYXNz"};
    constexpr std::string_view address{R"({"jsonrpc":"2.0","id":1,"method":"getnewaddress"})"};
    constexpr std::string_view mine{R"({"jsonrpc":"2.0","id":2,"method":"generateregtestblock"})"};
    ASSERT_NE(runtime->Rpc().HandleHttpPost({"POST", auth, address}).body.find("\"result\""),
              std::string::npos);
    const auto before = runtime->View();
    ASSERT_TRUE(before);
    const auto prior = Read(Selector());
    ASSERT_FALSE(prior.empty());

    observer_->FailStoreForTesting(nova::storage::GenerationFailurePoint::kAfterWallet);
    const auto rejected = runtime->Rpc().HandleHttpPost({"POST", auth, mine});
    EXPECT_EQ(rejected.status, 200U);
    EXPECT_NE(rejected.body.find("regtest block generation failed"), std::string::npos);
    EXPECT_EQ(rejected.body.find("may have committed"), std::string::npos);
    const auto unchanged = runtime->View();
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->tip, before->tip);
    EXPECT_EQ(unchanged->chain_work, before->chain_work);
    EXPECT_EQ(Read(Selector()), prior);

    observer_->FailAdvanceForTesting();
    const auto uncertain = runtime->Rpc().HandleHttpPost({"POST", auth, mine});
    EXPECT_EQ(uncertain.status, 200U);
    EXPECT_NE(uncertain.body.find("operation outcome may have committed"), std::string::npos);
    EXPECT_FALSE(runtime->Ready());
    const auto committed = Read(Selector());
    EXPECT_NE(committed, prior);
    const auto refused = runtime->Rpc().HandleHttpPost({"POST", auth, mine});
    EXPECT_NE(refused.body.find("node state unavailable"), std::string::npos);
    EXPECT_EQ(Read(Selector()), committed);
    runtime.reset();

    std::optional<nova::node::DurableSessionView> first_recovery;
    for (unsigned restart = 0U; restart < 2U; ++restart) {
        node_ = Open();
        ASSERT_TRUE(node_);
        const auto recovered = node_->View();
        ASSERT_TRUE(recovered);
        EXPECT_EQ(recovered->height, before->height + 1U);
        EXPECT_NE(recovered->tip, before->tip);
        EXPECT_LT(before->chain_work, recovered->chain_work);
        EXPECT_EQ(recovered->receiving_address_count, before->receiving_address_count);
        EXPECT_EQ(Read(Selector()), committed);
        if (first_recovery) {
            EXPECT_EQ(recovered->tip, first_recovery->tip);
            EXPECT_EQ(recovered->chain_work, first_recovery->chain_work);
        }
        first_recovery = recovered;
        node_.reset();
    }
}

TEST_F(DurableShutdownTest, RpcWalletSendRecoversTransactionAfterUncertainPublication)
{
    CheckRpcTransactionRecovery(true);
}

TEST_F(DurableShutdownTest, RpcTransactionSubmissionRecoversAfterUncertainPublication)
{
    CheckRpcTransactionRecovery(false);
}

TEST_F(DurableShutdownTest, CandidateCloseErrorsRejectNodeWritersWithoutPublishingAcrossRestarts)
{
    const auto address = node_->GenerateAddress();
    ASSERT_TRUE(address);
    ASSERT_TRUE(node_->Mine());
    const auto before = node_->View();
    const auto wallet = node_->Session().WalletView();
    ASSERT_TRUE(before);
    ASSERT_TRUE(wallet);
    ASSERT_EQ(wallet->coins.size(), 1U);
    const auto root = Read(Selector());
    const auto unchanged = [&] {
        ASSERT_TRUE(node_->Ready());
        const auto after = node_->View();
        const auto recovered = node_->Session().WalletView();
        ASSERT_TRUE(after);
        ASSERT_TRUE(recovered);
        EXPECT_EQ(Read(Selector()), root);
        EXPECT_EQ(after->tip, before->tip);
        EXPECT_EQ(after->height, before->height);
        EXPECT_EQ(after->chain_work, before->chain_work);
        EXPECT_EQ(after->next_time, before->next_time);
        EXPECT_EQ(after->mempool_size, before->mempool_size);
        EXPECT_EQ(after->mempool_bytes, before->mempool_bytes);
        EXPECT_EQ(after->receiving_address_count, before->receiving_address_count);
        EXPECT_EQ(recovered->confirmed, wallet->confirmed);
        EXPECT_EQ(recovered->unconfirmed, wallet->unconfirmed);
        ASSERT_EQ(recovered->coins.size(), 1U);
        const auto& coin = recovered->coins.front();
        const auto& old = wallet->coins.front();
        EXPECT_EQ(coin.key, old.key);
        EXPECT_EQ(coin.output.value, old.output.value);
        EXPECT_EQ(coin.output.script_pubkey, old.output.script_pubkey);
        EXPECT_EQ(coin.height, old.height);
        EXPECT_EQ(coin.is_coinbase, old.is_coinbase);
        EXPECT_EQ(coin.confirmed, old.confirmed);
        EXPECT_EQ(coin.pending_spend, old.pending_spend);
        const auto utxo = node_->Session().ReadCoin(old.key);
        ASSERT_EQ(utxo.error, nova::chain::CoinReadError::kNone);
        ASSERT_TRUE(utxo.coin);
        EXPECT_EQ(utxo.coin->output.value, old.output.value);
        EXPECT_EQ(utxo.coin->output.script_pubkey, old.output.script_pubkey);
    };
    using Point = nova::storage::GenerationFailurePoint;
    for (const auto point : {Point::kJournalClose, Point::kWalletClose, Point::kMempoolClose,
                             Point::kDescriptorClose, Point::kRootClose}) {
        SCOPED_TRACE(static_cast<unsigned>(point));
        observer_->FailStoreForTesting(point);
        EXPECT_FALSE(node_->GenerateAddress());
        unchanged();
        observer_->FailStoreForTesting(point);
        EXPECT_FALSE(node_->Mine());
        unchanged();
        observer_->FailStoreForTesting(point);
        EXPECT_FALSE(node_->Send({address->public_key_hash, 1}));
        EXPECT_FALSE(node_->UncertainPayment());
        unchanged();
        for (unsigned restart = 0U; restart < 2U; ++restart) {
            ASSERT_TRUE(node_->Shutdown());
            node_.reset();
            node_ = Open();
            ASSERT_NE(node_, nullptr);
            unchanged();
        }
    }
    const auto next = node_->GenerateAddress();
    ASSERT_TRUE(next);
    EXPECT_EQ(next->index, 1U);
    ASSERT_TRUE(node_->Mine());
    const auto payment = node_->Send({address->public_key_hash, 1});
    ASSERT_TRUE(payment);
    ASSERT_TRUE(node_->Shutdown());
    node_.reset();
    node_ = Open();
    ASSERT_NE(node_, nullptr);
    const auto after = node_->View();
    ASSERT_TRUE(after);
    EXPECT_EQ(after->height, 2U);
    EXPECT_EQ(after->receiving_address_count, 2U);
    EXPECT_EQ(after->mempool_size, 1U);
    const auto pending = node_->Session().FindMempoolEntry(*payment);
    EXPECT_TRUE(pending.available && pending.entry);
}
TEST_F(DurableShutdownTest, WalletScratchFailuresPreserveEveryWriterAndRepeatedRecovery)
{
    const auto address = node_->GenerateAddress();
    ASSERT_TRUE(address);
    ASSERT_TRUE(node_->Mine());
    const std::array<nova::wallet::Recipient, 1U> recipients{{{address->public_key_hash, 1}}};
    const auto built = node_->Session().BuildTransaction(recipients);
    ASSERT_TRUE(built && built->built);
    const auto transaction = built->built->transaction;
    const auto id =
        transaction.TxId(node_->Session().network_params().block_limits.transaction_limits);
    ASSERT_TRUE(id);
    const auto before = node_->View();
    const auto wallet_before = node_->Session().WalletView();
    ASSERT_TRUE(before && wallet_before);
    ASSERT_EQ(wallet_before->coins.size(), 1U);
    const auto original = wallet_before->coins.front();
    const auto root = Read(Selector());
    const auto unchanged = [&] {
        ASSERT_TRUE(node_->Ready());
        const auto view = node_->View();
        const auto wallet = node_->Session().WalletView();
        ASSERT_TRUE(view && wallet);
        EXPECT_EQ(view->tip, before->tip);
        EXPECT_EQ(view->height, before->height);
        EXPECT_EQ(view->chain_work, before->chain_work);
        EXPECT_EQ(view->next_time, before->next_time);
        EXPECT_EQ(view->receiving_address_count, before->receiving_address_count);
        EXPECT_EQ(view->mempool_size, 0U);
        EXPECT_EQ(view->mempool_bytes, 0U);
        EXPECT_EQ(wallet->confirmed, wallet_before->confirmed);
        EXPECT_EQ(wallet->unconfirmed, wallet_before->unconfirmed);
        ASSERT_EQ(wallet->coins.size(), 1U);
        const auto& coin = wallet->coins.front();
        EXPECT_EQ(coin.key, original.key);
        EXPECT_EQ(coin.output.value, original.output.value);
        EXPECT_EQ(coin.output.script_pubkey, original.output.script_pubkey);
        EXPECT_EQ(coin.height, original.height);
        EXPECT_EQ(coin.is_coinbase, original.is_coinbase);
        EXPECT_EQ(coin.confirmed, original.confirmed);
        EXPECT_EQ(coin.pending_spend, original.pending_spend);
        const auto utxo = node_->Session().ReadCoin(original.key);
        ASSERT_EQ(utxo.error, nova::chain::CoinReadError::kNone);
        ASSERT_TRUE(utxo.coin);
        EXPECT_EQ(utxo.coin->output.value, original.output.value);
        EXPECT_EQ(utxo.coin->output.script_pubkey, original.output.script_pubkey);
        const auto pending = node_->Session().FindMempoolEntry(*id);
        EXPECT_TRUE(pending.available && !pending.entry);
        EXPECT_EQ(Read(Selector()), root);
    };
    using Stage = nova::wallet::detail::SensitiveFailureStage;
    for (const auto stage : {Stage::kAllocation, Stage::kEncryptionScratch,
                             Stage::kDecryptionScratch, Stage::kFileStaging}) {
        for (unsigned writer = 0; writer < 4U; ++writer) {
            SCOPED_TRACE(static_cast<unsigned>(stage));
            SCOPED_TRACE(writer);
            const auto metrics = node_->Metrics();
            {
                nova::wallet::detail::ScopedSensitiveFailureForTesting fault{stage};
                switch (writer) {
                case 0U:
                    EXPECT_FALSE(node_->GenerateAddress());
                    break;
                case 1U:
                    EXPECT_FALSE(node_->Mine());
                    break;
                case 2U:
                    EXPECT_FALSE(node_->Send(recipients.front()));
                    break;
                case 3U:
                    EXPECT_NE(node_->ReceiveTransaction(transaction),
                              nova::node::DurableSessionError::kNone);
                    break;
                }
                EXPECT_TRUE(fault.triggered());
            }
            EXPECT_EQ(Read(Selector()), root);
            EXPECT_EQ(node_->Metrics().blocks_accepted, metrics.blocks_accepted);
            EXPECT_EQ(node_->Metrics().blocks_rejected, metrics.blocks_rejected);
            EXPECT_EQ(node_->Metrics().transactions_accepted, metrics.transactions_accepted);
            EXPECT_EQ(node_->Metrics().transactions_rejected, metrics.transactions_rejected);
            if (node_->Ready())
                unchanged();
            else
                EXPECT_FALSE(node_->View()); // Explicit unavailable state is not a rejection proof.
            for (unsigned restart = 0; restart < 2U; ++restart) {
                const bool available = node_->Ready();
                EXPECT_EQ(node_->Shutdown(), available);
                node_.reset();
                node_ = Open();
                ASSERT_NE(node_, nullptr);
                unchanged();
            }
        }
    }
    const auto next = node_->GenerateAddress();
    ASSERT_TRUE(next);
    EXPECT_EQ(next->index, 1U);
    const auto payment = node_->Send(recipients.front());
    ASSERT_TRUE(payment); // Prior receiving key still authorizes the original coin.
    ASSERT_TRUE(node_->Shutdown());
    node_.reset();
    node_ = Open();
    ASSERT_NE(node_, nullptr);
    const auto pending = node_->Session().FindMempoolEntry(*payment);
    EXPECT_TRUE(pending.available && pending.entry);
}

TEST_F(DurableShutdownTest, WriteAndSyncErrorsRejectWriterIngressWithoutLosingState)
{
    const auto address = node_->GenerateAddress();
    ASSERT_TRUE(address);
    ASSERT_TRUE(node_->Mine());
    const std::array<nova::wallet::Recipient, 1U> recipients{{{address->public_key_hash, 1}}};
    const auto built = node_->Session().BuildTransaction(recipients);
    ASSERT_TRUE(built && built->built);
    const auto transaction = built->built->transaction;
    const auto id =
        transaction.TxId(node_->Session().network_params().block_limits.transaction_limits);
    ASSERT_TRUE(id);
    const auto before = node_->View();
    const auto wallet = node_->Session().WalletView();
    ASSERT_TRUE(before);
    ASSERT_TRUE(wallet);
    ASSERT_EQ(wallet->coins.size(), 1U);
    const auto root = Read(Selector());
    const auto unchanged = [&] {
        ASSERT_TRUE(node_->Ready());
        const auto current = node_->View();
        const auto recovered = node_->Session().WalletView();
        ASSERT_TRUE(current);
        ASSERT_TRUE(recovered);
        EXPECT_EQ(Read(Selector()), root);
        EXPECT_EQ(current->tip, before->tip);
        EXPECT_EQ(current->height, before->height);
        EXPECT_EQ(current->chain_work, before->chain_work);
        EXPECT_EQ(current->next_time, before->next_time);
        EXPECT_EQ(current->mempool_size, 0U);
        EXPECT_EQ(current->mempool_bytes, 0U);
        EXPECT_EQ(current->receiving_address_count, before->receiving_address_count);
        EXPECT_EQ(recovered->confirmed, wallet->confirmed);
        EXPECT_EQ(recovered->unconfirmed, wallet->unconfirmed);
        ASSERT_EQ(recovered->coins.size(), 1U);
        const auto& old = wallet->coins.front();
        const auto& coin = recovered->coins.front();
        EXPECT_EQ(coin.key, old.key);
        EXPECT_EQ(coin.output.value, old.output.value);
        EXPECT_EQ(coin.output.script_pubkey, old.output.script_pubkey);
        EXPECT_EQ(coin.height, old.height);
        EXPECT_EQ(coin.is_coinbase, old.is_coinbase);
        EXPECT_EQ(coin.confirmed, old.confirmed);
        EXPECT_EQ(coin.pending_spend, old.pending_spend);
        const auto pending = node_->Session().FindMempoolEntry(*id);
        EXPECT_TRUE(pending.available);
        EXPECT_FALSE(pending.entry);
        const auto utxo = node_->Session().ReadCoin(old.key);
        ASSERT_EQ(utxo.error, nova::chain::CoinReadError::kNone);
        ASSERT_TRUE(utxo.coin);
        EXPECT_EQ(utxo.coin->output.value, old.output.value);
        EXPECT_EQ(utxo.coin->output.script_pubkey, old.output.script_pubkey);
    };
    const auto verify_rejection = [&] {
        EXPECT_TRUE(observer_->StoreIoFaultTriggeredForTesting());
        unchanged();
        for (unsigned restart = 0U; restart < 2U; ++restart) {
            SCOPED_TRACE(restart);
            ASSERT_TRUE(node_->Shutdown());
            node_.reset();
            node_ = Open();
            ASSERT_NE(node_, nullptr);
            unchanged();
        }
    };
    using Target = nova::storage::GenerationIoTarget;
    using Fault = nova::storage::GenerationIoFault;
    for (const auto target : {Target::kJournal, Target::kWallet, Target::kMempool,
                              Target::kDescriptor, Target::kRoot}) {
        for (const auto fault : {Fault::kZeroProgress, Fault::kNoSpace, Fault::kPrefixThenNoSpace,
                                 Fault::kSyncFailure}) {
            SCOPED_TRACE(static_cast<unsigned>(target));
            SCOPED_TRACE(static_cast<unsigned>(fault));
            {
                SCOPED_TRACE("address generation");
                observer_->SetStoreIoFaultForTesting(target, fault);
                EXPECT_FALSE(node_->GenerateAddress());
                ASSERT_NO_FATAL_FAILURE(verify_rejection());
            }
            {
                SCOPED_TRACE("mining");
                observer_->SetStoreIoFaultForTesting(target, fault);
                EXPECT_FALSE(node_->Mine());
                ASSERT_NO_FATAL_FAILURE(verify_rejection());
            }
            {
                SCOPED_TRACE("wallet payment");
                observer_->SetStoreIoFaultForTesting(target, fault);
                EXPECT_FALSE(node_->Send({address->public_key_hash, 1}));
                EXPECT_FALSE(node_->UncertainPayment());
                ASSERT_NO_FATAL_FAILURE(verify_rejection());
            }
            {
                SCOPED_TRACE("transaction ingress");
                observer_->SetStoreIoFaultForTesting(target, fault);
                EXPECT_EQ(node_->ReceiveTransaction(transaction),
                          nova::node::DurableSessionError::kStorage);
                ASSERT_NO_FATAL_FAILURE(verify_rejection());
            }
        }
    }
    EXPECT_EQ(node_->ReceiveTransaction(transaction), nova::node::DurableSessionError::kNone);
    ASSERT_TRUE(node_->Shutdown());
    node_.reset();
    node_ = Open();
    ASSERT_NE(node_, nullptr);
    const auto pending = node_->Session().FindMempoolEntry(*id);
    EXPECT_TRUE(pending.available && pending.entry);
    const auto current = node_->View();
    ASSERT_TRUE(current);
    EXPECT_EQ(current->mempool_size, 1U);
    EXPECT_EQ(current->tip, before->tip);
}
} // namespace
