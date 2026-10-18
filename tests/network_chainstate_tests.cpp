#include <gtest/gtest.h>

#include "chain/network_chainstate.hpp"
#include "node/network_selection.hpp"
#include "storage/block_journal.hpp"
#include "wallet/wallet.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace
{
using namespace nova;

std::shared_ptr<chain::FixedValidationTimeSource> Clock()
{
    return std::make_shared<chain::FixedValidationTimeSource>(1'704'160'000U);
}

TEST(NetworkInitialization, FrozenTestnetCommitmentAndConsumerPayload)
{
    const auto& network = consensus::TestnetNetworkParams();
    auto state = chain::NetworkChainInitializer::Create(network.id, Clock());
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->chain->active_tip(), network.genesis_hash);
    EXPECT_EQ(state->chain->active_height(), 0U);
    const auto indexes = state->chain->GetActiveBlockIndexes();
    ASSERT_TRUE(indexes.has_value());
    ASSERT_EQ(indexes->size(), 1U);
    ASSERT_TRUE(indexes->front().block.has_value());
    EXPECT_FALSE(indexes->front().connected.has_value());
    EXPECT_EQ(primitives::ComputeBlockHash(indexes->front().block->header), network.genesis_hash);
    std::size_t count = 0U;
    primitives::Amount total = 0;
    for (const auto& transaction : network.genesis_block.transactions) {
        const auto id = transaction.TxId(network.block_limits.transaction_limits);
        ASSERT_TRUE(id.has_value());
        for (std::size_t i = 0U; i < transaction.outputs.size(); ++i) {
            const auto coin = state->utxos->GetCoin({*id, static_cast<std::uint32_t>(i)});
            ASSERT_TRUE(coin.has_value());
            EXPECT_EQ(coin->output.value, transaction.outputs[i].value);
            EXPECT_EQ(coin->output.script_pubkey, transaction.outputs[i].script_pubkey);
            EXPECT_EQ(coin->height, 0U);
            EXPECT_TRUE(coin->is_coinbase);
            total += coin->output.value;
            ++count;
        }
    }
    EXPECT_EQ(state->utxos->size(), count);
    EXPECT_EQ(total, 1'000'000'000'000'000LL);
    EXPECT_EQ(chain::NetworkValidationParams(network).coinbase_maturity, 0U);
    EXPECT_FALSE(network.enabled);
    EXPECT_NE(node::SelectNetwork(network.id).error, node::NetworkSelectionError::kNone);
    EXPECT_FALSE(consensus::MainnetNetworkParams().enabled);
}

TEST(NetworkInitialization, RegtestUnchangedAndInvalidConstructionFailsClosed)
{
    auto state = chain::NetworkChainInitializer::Create(consensus::NetworkId::kRegtest, Clock());
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->utxos->size(), 0U);
    const auto anchor = state->chain->GetBlockIndex(consensus::RegtestNetworkParams().genesis_hash);
    ASSERT_TRUE(anchor.has_value());
    EXPECT_FALSE(anchor->block.has_value());
    EXPECT_FALSE(chain::NetworkChainInitializer::Create(consensus::NetworkId::kMainnet, Clock()));
    EXPECT_FALSE(
        chain::NetworkChainInitializer::Create(static_cast<consensus::NetworkId>(255U), Clock()));
    EXPECT_FALSE(chain::NetworkChainInitializer::Create(consensus::NetworkId::kTestnet, nullptr));
}

TEST(NetworkInitialization, SpentToEmptyCannotBeReseededByDuplicateGenesis)
{
    const auto& network = consensus::TestnetNetworkParams();
    auto state = chain::NetworkChainInitializer::Create(network.id, Clock());
    ASSERT_TRUE(state.has_value());
    const auto id =
        network.genesis_block.transactions.front().TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(id.has_value());
    // Primitive mutation tests lifecycle ownership, not a creator-authorized spend.
    ASSERT_TRUE(state->utxos->SpendCoin({*id, 0U}).has_value());
    ASSERT_EQ(state->utxos->size(), 0U);
    EXPECT_NE(state->chain->AcceptBlock(network.genesis_block).error, chain::ChainError::kNone);
    EXPECT_EQ(state->utxos->size(), 0U);
    auto independent = chain::NetworkChainInitializer::Create(network.id, Clock());
    ASSERT_TRUE(independent.has_value());
    EXPECT_EQ(independent->utxos->size(), 1U);
    EXPECT_EQ(state->utxos->size(), 0U);
}

TEST(NetworkInitialization, InjectedAllocationFailurePublishesNothingAndDoesNotAffectExistingState)
{
    using Initializer = chain::NetworkChainInitializer;
    auto existing = Initializer::Create(consensus::NetworkId::kTestnet, Clock());
    ASSERT_TRUE(existing.has_value());
    const auto& network = consensus::TestnetNetworkParams();
    const auto id =
        network.genesis_block.transactions.front().TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(existing->utxos->SpendCoin({*id, 0U}).has_value());
    for (const auto point :
         {Initializer::FailurePoint::kBeforeSeed, Initializer::FailurePoint::kAfterSeed}) {
        Initializer::SetFailurePointForTesting(point);
        EXPECT_FALSE(Initializer::Create(network.id, Clock()).has_value());
        EXPECT_EQ(existing->utxos->size(), 0U);
        const auto fresh = Initializer::Create(network.id, Clock());
        ASSERT_TRUE(fresh.has_value());
        EXPECT_EQ(fresh->utxos->size(), 1U);
    }
    Initializer::SetFailurePointForTesting(Initializer::FailurePoint::kNone);
}

// Synthetic key 1 and synthetic genesis: never the frozen creator key/commitment.
struct SyntheticFixture final {
    chain::ChainStateParams parameters;
    chain::ChainAnchor anchor;
    primitives::Transaction genesis;
    chain::UTXOKey key;
    crypto::PrivateKey signing_key;
    crypto::PublicKey public_key;
};

std::optional<SyntheticFixture> Synthetic()
{
    crypto::PrivateKey::Bytes bytes{};
    bytes.back() = 1U;
    auto key = crypto::PrivateKey::FromBytes(bytes);
    if (!key.has_value())
        return std::nullopt;
    const auto pub = key->DerivePublicKey();
    const auto hash =
        pub.has_value() ? crypto::Hash160Digest(pub->SerializeCompressed()) : std::nullopt;
    if (!pub.has_value() || !hash.has_value())
        return std::nullopt;
    std::vector<std::uint8_t> script{0x76U, 0xa9U, 0x14U};
    script.insert(script.end(), hash->begin(), hash->end());
    script.insert(script.end(), {0x88U, 0xacU});
    auto parameters = chain::NetworkValidationParams(consensus::RegtestNetworkParams());
    primitives::Transaction genesis{1,
                                    {{{crypto::Hash256{}, 0xffff'ffffU}, {1U, 0U}, 0U}},
                                    {{100 * consensus::COIN, script}},
                                    0U};
    const auto id = genesis.TxId(parameters.block_limits.transaction_limits);
    const auto target = consensus::TargetFromCompact(0x207fffffU);
    if (!id.has_value() || !target.target.has_value())
        return std::nullopt;
    const auto work = consensus::CalculateWork(*target.target);
    if (!work.has_value())
        return std::nullopt;
    return SyntheticFixture{
        {parameters, {600U, 1U, 600U, true, true}, 11U, Clock()},
        {*id, {}, 0U, 1'000U, *target.target, *work, chain::ChainWork::FromPerBlockWork(*work)},
        genesis,
        {*id, 0U},
        std::move(*key),
        *pub};
}

std::unique_ptr<chain::ChainState> SyntheticState(const SyntheticFixture& fixture,
                                                  chain::UTXOSet& utxos)
{
    if (utxos
            .ApplyBlock(std::span{&fixture.genesis, 1U}, 0U,
                        fixture.parameters.block_validation.block_limits.transaction_limits)
            .error != chain::UTXOError::kNone)
        return nullptr;
    return chain::ChainState::Create(fixture.parameters, fixture.anchor, utxos);
}

std::optional<primitives::Transaction> Spend(const SyntheticFixture& fixture,
                                             const primitives::Amount output = 99 * consensus::COIN)
{
    primitives::Transaction tx{1,
                               {{{fixture.key.transaction_id, 0U}, {}, 0xffff'ffffU}},
                               {{output, fixture.genesis.outputs.front().script_pubkey}},
                               0U};
    const auto digest = chain::ComputeSignatureHash(
        tx, 0U, fixture.genesis.outputs.front().script_pubkey,
        fixture.parameters.block_validation.block_limits.transaction_limits, 1U);
    const auto signature = digest.has_value() ? fixture.signing_key.Sign(*digest) : std::nullopt;
    if (!signature.has_value())
        return std::nullopt;
    const auto der = signature->SerializeDer();
    tx.inputs.front().script_sig.push_back(static_cast<std::uint8_t>(der.size() + 1U));
    tx.inputs.front().script_sig.insert(tx.inputs.front().script_sig.end(), der.begin(), der.end());
    tx.inputs.front().script_sig.insert(tx.inputs.front().script_sig.end(), {1U, 33U});
    const auto pub = fixture.public_key.SerializeCompressed();
    tx.inputs.front().script_sig.insert(tx.inputs.front().script_sig.end(), pub.begin(), pub.end());
    return tx;
}

std::optional<primitives::Block>
Child(const SyntheticFixture& fixture, const crypto::Hash256& parent, std::uint32_t height,
      std::uint32_t time, const std::optional<primitives::Transaction>& spend = std::nullopt)
{
    const auto subsidy =
        consensus::GetBlockSubsidy(height, fixture.parameters.block_validation.chain_parameters);
    if (!subsidy.has_value())
        return std::nullopt;
    primitives::Transaction coinbase{
        1,
        {{{{}, 0xffff'ffffU},
          {1U, static_cast<std::uint8_t>(height), static_cast<std::uint8_t>(time & 255U)},
          0U}},
        {{*subsidy, {0x51U}}},
        0U};
    primitives::Block block{{1, parent, {}, time, 0x207fffffU, 0U}, {coinbase}};
    if (spend.has_value())
        block.transactions.push_back(*spend);
    const auto root = primitives::ComputeMerkleRoot(
        block.transactions, fixture.parameters.block_validation.block_limits.transaction_limits);
    if (!root.has_value())
        return std::nullopt;
    block.header.merkle_root = *root;
    const auto mined = consensus::MineRegtestBlock(block.header, 4096U);
    if (!mined.has_value())
        return std::nullopt;
    block.header = mined->header;
    return block;
}

TEST(NetworkInitialization, SyntheticSignedSpendRollbackReorgAndRestartReplay)
{
    auto fixture = Synthetic();
    ASSERT_TRUE(fixture.has_value());
    chain::UTXOSet utxos;
    auto state = SyntheticState(*fixture, utxos);
    ASSERT_NE(state, nullptr);
    const auto spend = Spend(*fixture);
    ASSERT_TRUE(spend.has_value());
    EXPECT_EQ(chain::ValidateTransactionAgainstUTXOSet(*spend, {1U, 1000U},
                                                       fixture->parameters.block_validation, utxos)
                  .error,
              chain::BlockValidationError::kNone);
    auto immature = fixture->parameters.block_validation;
    immature.coinbase_maturity = 2U;
    EXPECT_EQ(chain::ValidateTransactionAgainstUTXOSet(*spend, {1U, 1000U}, immature, utxos).error,
              chain::BlockValidationError::kImmatureCoinbase);
    const auto invalid = Spend(*fixture, 101 * consensus::COIN);
    ASSERT_TRUE(invalid.has_value());
    EXPECT_EQ(chain::ValidateTransactionAgainstUTXOSet(*invalid, {1U, 1000U},
                                                       fixture->parameters.block_validation, utxos)
                  .error,
              chain::BlockValidationError::kInputValueTooLow);
    EXPECT_TRUE(utxos.HaveCoin(fixture->key));
    auto bad_signature = *spend;
    bad_signature.inputs.front().script_sig[10] ^= 1U;
    EXPECT_NE(chain::ValidateTransactionAgainstUTXOSet(bad_signature, {1U, 1000U},
                                                       fixture->parameters.block_validation, utxos)
                  .error,
              chain::BlockValidationError::kNone);
    auto wrong_key = *spend;
    crypto::PrivateKey::Bytes other_bytes{};
    other_bytes.back() = 2U;
    const auto other = crypto::PrivateKey::FromBytes(other_bytes);
    ASSERT_TRUE(other.has_value());
    const auto other_pub = other->DerivePublicKey();
    ASSERT_TRUE(other_pub.has_value());
    const auto serialized = other_pub->SerializeCompressed();
    std::copy(serialized.begin(), serialized.end(), wrong_key.inputs.front().script_sig.end() - 33);
    EXPECT_NE(chain::ValidateTransactionAgainstUTXOSet(wrong_key, {1U, 1000U},
                                                       fixture->parameters.block_validation, utxos)
                  .error,
              chain::BlockValidationError::kNone);
    const auto a = Child(*fixture, fixture->anchor.hash, 1U, 1001U, spend);
    ASSERT_TRUE(a.has_value());
    auto accepted = state->AcceptBlock(*a);
    ASSERT_EQ(accepted.error, chain::ChainError::kNone);
    ASSERT_TRUE(accepted.block_hash.has_value());
    EXPECT_FALSE(utxos.HaveCoin(fixture->key));
    EXPECT_EQ(chain::ValidateTransactionAgainstUTXOSet(*spend, {2U, 1001U},
                                                       fixture->parameters.block_validation, utxos)
                  .error,
              chain::BlockValidationError::kMissingUtxo);
    // Equivalent to node rollback after an uncommitted WAL write.
    ASSERT_EQ(state->RollbackAcceptedBlock(*accepted.block_hash, fixture->anchor.hash).error,
              chain::ChainError::kNone);
    ASSERT_TRUE(utxos.GetCoin(fixture->key).has_value());
    EXPECT_TRUE(utxos.GetCoin(fixture->key)->is_coinbase);
    ASSERT_EQ(state->AcceptBlock(*a).error, chain::ChainError::kNone);
    const auto b = Child(*fixture, fixture->anchor.hash, 1U, 1002U);
    ASSERT_TRUE(b.has_value());
    const auto b_hash = primitives::ComputeBlockHash(b->header);
    ASSERT_TRUE(b_hash.has_value());
    ASSERT_EQ(state->AcceptBlock(*b).error, chain::ChainError::kNone);
    const auto c = Child(*fixture, *b_hash, 2U, 1003U);
    ASSERT_TRUE(c.has_value());
    ASSERT_EQ(state->AcceptBlock(*c).error, chain::ChainError::kNone);
    EXPECT_TRUE(utxos.HaveCoin(fixture->key));
    EXPECT_EQ(utxos.GetCoin(fixture->key)->height, 0U);
    const auto c_hash = primitives::ComputeBlockHash(c->header);
    ASSERT_TRUE(c_hash.has_value());
    const auto d = Child(*fixture, *c_hash, 3U, 1004U, spend);
    ASSERT_TRUE(d.has_value());
    ASSERT_EQ(state->AcceptBlock(*d).error, chain::ChainError::kNone);
    EXPECT_FALSE(utxos.HaveCoin(fixture->key));
    // A separate, correctly signed competing spend wins only on a longer branch.
    const auto competing = Spend(*fixture, 98 * consensus::COIN);
    ASSERT_TRUE(competing.has_value());
    std::vector<primitives::Block> competing_branch;
    auto parent = fixture->anchor.hash;
    for (std::uint32_t height = 1U; height <= 4U; ++height) {
        const auto block = Child(*fixture, parent, height, 1100U + height,
                                 height == 1U ? competing : std::nullopt);
        ASSERT_TRUE(block.has_value());
        const auto hash = primitives::ComputeBlockHash(block->header);
        ASSERT_TRUE(hash.has_value());
        parent = *hash;
        ASSERT_EQ(state->AcceptBlock(*block).error, chain::ChainError::kNone);
        competing_branch.push_back(*block);
    }
    EXPECT_EQ(state->active_tip(), parent);
    EXPECT_EQ(state->active_height(), 4U);
    EXPECT_FALSE(utxos.HaveCoin(fixture->key));
    const auto original_spend_id =
        spend->TxId(fixture->parameters.block_validation.block_limits.transaction_limits);
    const auto competing_id =
        competing->TxId(fixture->parameters.block_validation.block_limits.transaction_limits);
    ASSERT_TRUE(original_spend_id.has_value());
    ASSERT_TRUE(competing_id.has_value());
    EXPECT_FALSE(utxos.HaveCoin({*original_spend_id, 0U}));
    const auto winner_coin = utxos.GetCoin({*competing_id, 0U});
    ASSERT_TRUE(winner_coin.has_value());
    EXPECT_EQ(winner_coin->output.value, 98 * consensus::COIN);
    EXPECT_EQ(utxos.size(), 5U); // four rewards plus one surviving allocation output
    // Fresh reconstruction seeds once, then replays all accepted child branches.
    chain::UTXOSet restarted_utxos;
    auto restarted = SyntheticState(*fixture, restarted_utxos);
    ASSERT_NE(restarted, nullptr);
    for (const auto& block : {*a, *b, *c, *d})
        ASSERT_EQ(restarted->AcceptBlock(block).error, chain::ChainError::kNone);
    for (const auto& block : competing_branch)
        ASSERT_EQ(restarted->AcceptBlock(block).error, chain::ChainError::kNone);
    EXPECT_EQ(restarted->active_tip(), state->active_tip());
    const auto original_tip = state->GetBlockIndex(state->active_tip());
    const auto replayed_tip = restarted->GetBlockIndex(restarted->active_tip());
    ASSERT_TRUE(original_tip.has_value());
    ASSERT_TRUE(replayed_tip.has_value());
    EXPECT_EQ(original_tip->chain_work, replayed_tip->chain_work);
    EXPECT_EQ(restarted_utxos.size(), utxos.size());
    EXPECT_FALSE(restarted_utxos.HaveCoin(fixture->key));
    EXPECT_NE(restarted->AcceptBlock(*d).error, chain::ChainError::kNone);
    EXPECT_FALSE(restarted_utxos.HaveCoin(fixture->key));
}

TEST(NetworkInitialization, AtomicSeedBatchFailureDoesNotLeakPartialOutputs)
{
    auto fixture = Synthetic();
    ASSERT_TRUE(fixture.has_value());
    chain::UTXOSet utxos;
    const std::vector<primitives::Transaction> duplicates{fixture->genesis, fixture->genesis};
    EXPECT_NE(utxos
                  .ApplyBlock(duplicates, 0U,
                              fixture->parameters.block_validation.block_limits.transaction_limits)
                  .error,
              chain::UTXOError::kNone);
    EXPECT_EQ(utxos.size(), 0U);
}

class InitializationJournalTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path() /
                ("nova-genesis-initialization-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(root_));
    }
    void TearDown() override
    {
        storage::BlockJournal::SetFailurePointForTesting(storage::JournalFailurePoint::kNone);
        std::error_code error;
        if (!root_.empty())
            std::filesystem::remove_all(root_, error);
    }
    std::filesystem::path root_;
};

TEST_F(InitializationJournalTest, SyntheticWalFailureAndDurableReplayPreserveSpentSeed)
{
    auto fixture = Synthetic();
    ASSERT_TRUE(fixture.has_value());
    chain::UTXOSet utxos;
    auto state = SyntheticState(*fixture, utxos);
    ASSERT_NE(state, nullptr);
    auto journal =
        storage::BlockJournal::Open(root_, fixture->parameters.block_validation.block_limits);
    ASSERT_TRUE(journal.has_value());
    const auto block = Child(*fixture, fixture->anchor.hash, 1U, 1001U, Spend(*fixture));
    ASSERT_TRUE(block.has_value());
    const auto hash = primitives::ComputeBlockHash(block->header);
    ASSERT_TRUE(hash.has_value());
    storage::BlockJournal::SetFailurePointForTesting(storage::JournalFailurePoint::kBeforeWrite);
    EXPECT_NE(journal->Prepare(*block), storage::BlockJournalError::kNone);
    EXPECT_EQ(state->active_tip(), fixture->anchor.hash);
    EXPECT_TRUE(utxos.HaveCoin(fixture->key));
    ASSERT_EQ(journal->Prepare(*block), storage::BlockJournalError::kNone);
    ASSERT_EQ(state->AcceptBlock(*block).error, chain::ChainError::kNone);
    storage::BlockJournal::SetFailurePointForTesting(storage::JournalFailurePoint::kBeforeWrite);
    ASSERT_NE(journal->Commit(*hash), storage::BlockJournalError::kNone);
    ASSERT_FALSE(journal->IsCommitted(*hash));
    ASSERT_EQ(state->RollbackAcceptedBlock(*hash, fixture->anchor.hash).error,
              chain::ChainError::kNone);
    EXPECT_TRUE(utxos.HaveCoin(fixture->key));
    ASSERT_EQ(journal->Prepare(*block), storage::BlockJournalError::kNone);
    ASSERT_EQ(state->AcceptBlock(*block).error, chain::ChainError::kNone);
    ASSERT_EQ(journal->Commit(*hash), storage::BlockJournalError::kNone);
    auto reopened =
        storage::BlockJournal::Open(root_, fixture->parameters.block_validation.block_limits);
    ASSERT_TRUE(reopened.has_value());
    const auto loaded = reopened->Load();
    ASSERT_EQ(loaded.error, storage::BlockJournalError::kNone);
    ASSERT_EQ(loaded.blocks.size(), 1U);
    chain::UTXOSet replayed;
    auto restarted = SyntheticState(*fixture, replayed);
    ASSERT_NE(restarted, nullptr);
    for (const auto& saved : loaded.blocks)
        ASSERT_EQ(restarted->AcceptBlock(saved).error, chain::ChainError::kNone);
    EXPECT_EQ(restarted->active_tip(), state->active_tip());
    EXPECT_FALSE(replayed.HaveCoin(fixture->key));
}

TEST_F(InitializationJournalTest, GenesisChildCompatibilityRejectsWithoutRewritingOriginal)
{
    const auto& network = consensus::TestnetNetworkParams();
    auto journal = storage::BlockJournal::Open(root_, network.block_limits);
    ASSERT_TRUE(journal.has_value());
    ASSERT_EQ(journal->Prepare(network.genesis_block), storage::BlockJournalError::kNone);
    ASSERT_EQ(journal->Commit(network.genesis_hash), storage::BlockJournalError::kNone);
    const auto read_bytes = [&]() {
        std::ifstream input{journal->path(), std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
    };
    const auto before = read_bytes();
    auto state = chain::NetworkChainInitializer::Create(network.id, Clock());
    ASSERT_TRUE(state.has_value());
    const auto loaded = journal->Load();
    ASSERT_EQ(loaded.error, storage::BlockJournalError::kNone);
    ASSERT_EQ(loaded.blocks.size(), 1U);
    EXPECT_NE(state->chain->AcceptBlock(loaded.blocks.front()).error, chain::ChainError::kNone);
    EXPECT_EQ(state->utxos->size(), 1U);
    EXPECT_EQ(read_bytes(), before);
}

TEST_F(InitializationJournalTest, SyntheticWalletConsumerObservesAnchorSpendAndRollback)
{
    const auto limits = consensus::TestnetNetworkParams().block_limits.transaction_limits;
    auto wallet = wallet::Wallet::Create({limits, 1, 1U, true});
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    std::vector<std::uint8_t> script{0x76U, 0xa9U, 0x14U};
    script.insert(script.end(), address->public_key_hash.begin(), address->public_key_hash.end());
    script.insert(script.end(), {0x88U, 0xacU});
    // Ephemeral wallet key and synthetic allocation only, not the creator key.
    primitives::Transaction genesis{1, {{{{}, 0xffff'ffffU}, {1U, 0U}, 0U}}, {{100, script}}, 0U};
    const auto id = genesis.TxId(limits);
    ASSERT_TRUE(id.has_value());
    chain::BlockIndex anchor{};
    anchor.block = primitives::Block{{}, {genesis}};
    std::vector<chain::BlockIndex> active{anchor};
    ASSERT_TRUE(wallet->RebuildConfirmedState(active));
    EXPECT_EQ(wallet->ConfirmedBalance(), 100);
    ASSERT_EQ(wallet->ListUTXOs().size(), 1U);
    EXPECT_EQ(wallet->ListUTXOs().front().height, 0U);
    EXPECT_TRUE(wallet->ListUTXOs().front().is_coinbase);
    const std::vector<std::uint8_t> passphrase{1U, 2U, 3U, 4U};
    ASSERT_TRUE(wallet->SaveEncrypted(root_ / "synthetic-wallet.dat", passphrase));
    auto loaded_wallet = wallet::Wallet::LoadEncrypted({limits, 1, 1U, true},
                                                       root_ / "synthetic-wallet.dat", passphrase);
    ASSERT_NE(loaded_wallet, nullptr);
    ASSERT_TRUE(loaded_wallet->RebuildConfirmedState(active));
    EXPECT_EQ(loaded_wallet->ConfirmedBalance(), 100);
    ASSERT_TRUE(wallet->RebuildConfirmedState(active));
    EXPECT_EQ(wallet->ConfirmedBalance(), 100);
    primitives::Transaction spent{1, {{{*id, 0U}, {}, 0xffff'ffffU}}, {{99, {0x51U}}}, 0U};
    chain::BlockIndex child{};
    child.height = 1U;
    child.block = primitives::Block{{}, {spent}};
    active.push_back(child);
    ASSERT_TRUE(wallet->RebuildConfirmedState(active));
    EXPECT_EQ(wallet->ConfirmedBalance(), 0);
    active.pop_back();
    ASSERT_TRUE(wallet->RebuildConfirmedState(active));
    EXPECT_EQ(wallet->ConfirmedBalance(), 100);
    active.front().block->transactions.front().outputs.clear();
    EXPECT_FALSE(wallet->RebuildConfirmedState(active));
    EXPECT_EQ(wallet->ConfirmedBalance(), 100);
    auto frozen = chain::NetworkChainInitializer::Create(consensus::NetworkId::kTestnet, Clock());
    ASSERT_TRUE(frozen.has_value());
    const auto frozen_indexes = frozen->chain->GetActiveBlockIndexes();
    ASSERT_TRUE(frozen_indexes.has_value());
    ASSERT_TRUE(wallet->RebuildConfirmedState(*frozen_indexes));
    EXPECT_EQ(wallet->ConfirmedBalance(), 0);
    EXPECT_EQ(frozen->utxos->size(), 1U);
}

} // namespace
