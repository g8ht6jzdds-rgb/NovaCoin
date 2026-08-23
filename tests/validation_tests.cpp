#include <gtest/gtest.h>

#include "chain/validation.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace
{

using nova::chain::BlockValidationContext;
using nova::chain::BlockValidationError;
using nova::chain::BlockValidationParams;
using nova::chain::ConnectBlock;
using nova::chain::DisconnectBlock;
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

struct Scenario final {
    BlockValidationParams parameters;
    BlockValidationContext context;
    Block block;
    UTXOSet utxos;
    std::vector<std::uint8_t> locking_script;
    UTXOKey spent_key;
};

nova::crypto::Hash256 HashWithLastByte(const std::uint8_t value)
{
    nova::crypto::Hash256::Bytes bytes{};
    bytes.back() = value;
    return nova::crypto::Hash256{bytes};
}

std::optional<nova::crypto::PrivateKey> KeyFor(const std::uint8_t value)
{
    nova::crypto::PrivateKey::Bytes bytes{};
    bytes.back() = value;
    return nova::crypto::PrivateKey::FromBytes(bytes);
}

std::vector<std::uint8_t> P2pkhScript(const nova::crypto::PublicKey& public_key)
{
    const auto key_hash = nova::crypto::Hash160Digest(public_key.SerializeCompressed());
    EXPECT_TRUE(key_hash.has_value());
    std::vector<std::uint8_t> script{0x76U, 0xA9U, 0x14U};
    if (!key_hash.has_value()) {
        return script;
    }
    script.insert(script.end(), key_hash->begin(), key_hash->end());
    script.insert(script.end(), {0x88U, 0xACU});
    return script;
}

bool SignInput(Transaction& transaction, const nova::crypto::PrivateKey& signing_key,
               const nova::crypto::PublicKey& public_key,
               const std::span<const std::uint8_t> locking_script, const TransactionLimits& limits)
{
    const auto digest =
        nova::chain::ComputeSignatureHash(transaction, 0U, locking_script, limits, 1U);
    if (!digest.has_value()) {
        return false;
    }
    const auto signature = signing_key.Sign(*digest);
    if (!signature.has_value()) {
        return false;
    }
    const auto der = signature->SerializeDer();
    if (der.size() + 1U > 73U) {
        return false;
    }
    std::vector<std::uint8_t> script;
    script.reserve(der.size() + 36U);
    script.push_back(static_cast<std::uint8_t>(der.size() + 1U));
    script.insert(script.end(), der.begin(), der.end());
    script.push_back(0x01U);
    script.push_back(0x21U);
    const auto& serialized_key = public_key.SerializeCompressed();
    script.insert(script.end(), serialized_key.begin(), serialized_key.end());
    transaction.inputs.front().script_sig = std::move(script);
    return true;
}

bool MineHeader(nova::primitives::BlockHeader& header)
{
    const auto mined = nova::consensus::MineRegtestBlock(header, 4'096U);
    if (!mined.has_value()) {
        return false;
    }
    header = mined->header;
    return true;
}

bool FinalizeBlock(Block& block, const TransactionLimits& limits)
{
    const auto root = nova::primitives::ComputeMerkleRoot(block.transactions, limits);
    if (!root.has_value()) {
        return false;
    }
    block.header.merkle_root = *root;
    return MineHeader(block.header);
}

Transaction Coinbase(const nova::primitives::Amount value)
{
    return Transaction{1,
                       std::vector<TxInput>{TxInput{
                           OutPoint{nova::crypto::Hash256{}, 0xFFFF'FFFFU}, {0x01U, 0x0AU}, 0U}},
                       std::vector<TxOutput>{TxOutput{value, {0x51U}}}, 0U};
}

void PopulateScenario(Scenario& scenario, const bool seed_is_coinbase = false,
                      const std::uint32_t seed_height = 0U)
{
    constexpr TransactionLimits kTransactionLimits{MAX_MONEY, 100'000U, 8U, 8U, 128U};
    constexpr BlockLimits kBlockLimits{kTransactionLimits, 100'000U, 8U, 64U};
    scenario.parameters = BlockValidationParams{kBlockLimits,
                                                nova::consensus::RegtestPowParameters(),
                                                ChainParams{COIN, MAX_MONEY, INITIAL_SUBSIDY, 10U},
                                                2U,
                                                500'000'000U,
                                                0xFFFF'FFFFU,
                                                1U,
                                                600U};
    scenario.context = BlockValidationContext{HashWithLastByte(0xA0U), 10U, 1'000U, 1'500U};

    const auto private_key = KeyFor(1U);
    ASSERT_TRUE(private_key.has_value());
    const auto public_key = private_key->DerivePublicKey();
    ASSERT_TRUE(public_key.has_value());
    scenario.locking_script = P2pkhScript(*public_key);
    scenario.spent_key = UTXOKey{HashWithLastByte(0x11U), 0U};
    ASSERT_TRUE(scenario.utxos.AddCoin(
        scenario.spent_key, nova::chain::Coin{TxOutput{50LL * COIN, scenario.locking_script},
                                              seed_height, seed_is_coinbase}));

    Transaction spend{1,
                      std::vector<TxInput>{TxInput{OutPoint{scenario.spent_key.transaction_id,
                                                            scenario.spent_key.output_index},
                                                   {},
                                                   0xFFFF'FFFFU}},
                      std::vector<TxOutput>{TxOutput{49LL * COIN, scenario.locking_script}}, 0U};
    ASSERT_TRUE(
        SignInput(spend, *private_key, *public_key, scenario.locking_script, kTransactionLimits));

    scenario.block =
        Block{{1, scenario.context.parent_block_id, nova::crypto::Hash256{}, 1'200U, 0U},
              {Coinbase(26LL * COIN), std::move(spend)}};
    ASSERT_TRUE(FinalizeBlock(scenario.block, kTransactionLimits));
}

void ResignFirstSpend(Scenario& scenario, const std::uint8_t key_marker = 1U)
{
    const auto private_key = KeyFor(key_marker);
    ASSERT_TRUE(private_key.has_value());
    const auto locking_key = KeyFor(1U);
    ASSERT_TRUE(locking_key.has_value());
    const auto public_key = locking_key->DerivePublicKey();
    ASSERT_TRUE(public_key.has_value());
    ASSERT_TRUE(SignInput(scenario.block.transactions.at(1), *private_key, *public_key,
                          scenario.locking_script,
                          scenario.parameters.block_limits.transaction_limits));
    ASSERT_TRUE(FinalizeBlock(scenario.block, scenario.parameters.block_limits.transaction_limits));
}

TEST(BlockValidation, ConnectsAndDisconnectsOneSignedBlockAtomically)
{
    Scenario scenario;
    PopulateScenario(scenario);

    const auto connected =
        ConnectBlock(scenario.block, scenario.context, scenario.parameters, scenario.utxos);
    ASSERT_EQ(connected.error, BlockValidationError::kNone);
    ASSERT_TRUE(connected.connected.has_value());
    EXPECT_FALSE(scenario.utxos.HaveCoin(scenario.spent_key));
    EXPECT_EQ(scenario.utxos.size(), 2U);
    EXPECT_EQ(DisconnectBlock(*connected.connected, scenario.utxos), BlockValidationError::kNone);
    EXPECT_TRUE(scenario.utxos.HaveCoin(scenario.spent_key));
    EXPECT_EQ(scenario.utxos.size(), 1U);
}

TEST(BlockValidation, RejectsPreviousBlockMismatchBeforeStateMutation)
{
    Scenario scenario;
    PopulateScenario(scenario);
    scenario.block.header.previous_block_id = HashWithLastByte(0xA1U);
    EXPECT_EQ(
        ConnectBlock(scenario.block, scenario.context, scenario.parameters, scenario.utxos).error,
        BlockValidationError::kPreviousBlockMismatch);
    EXPECT_TRUE(scenario.utxos.HaveCoin(scenario.spent_key));
}

TEST(BlockValidation, RejectsProofOfWorkFailures)
{
    Scenario bad_pow;
    PopulateScenario(bad_pow);
    const auto target = nova::consensus::TargetFromCompact(bad_pow.block.header.bits,
                                                           bad_pow.parameters.pow_parameters);
    ASSERT_TRUE(target.target.has_value());
    bool found_invalid_nonce = false;
    for (std::uint32_t attempt = 0U; attempt < 32U; ++attempt) {
        ++bad_pow.block.header.nonce;
        const auto hash = nova::primitives::ComputeBlockHash(bad_pow.block.header);
        ASSERT_TRUE(hash.has_value());
        if (!nova::consensus::CheckProofOfWork(*hash, *target.target)) {
            found_invalid_nonce = true;
            break;
        }
    }
    ASSERT_TRUE(found_invalid_nonce);
    EXPECT_EQ(ConnectBlock(bad_pow.block, bad_pow.context, bad_pow.parameters, bad_pow.utxos).error,
              BlockValidationError::kBadProofOfWork);
}

TEST(BlockValidation, RejectsBadMerkleRootWithValidProofOfWork)
{
    Scenario scenario;
    PopulateScenario(scenario);
    scenario.block.header.merkle_root = HashWithLastByte(0x55U);
    ASSERT_TRUE(MineHeader(scenario.block.header));
    EXPECT_EQ(
        ConnectBlock(scenario.block, scenario.context, scenario.parameters, scenario.utxos).error,
        BlockValidationError::kBadMerkleRoot);
}

TEST(BlockValidation, RejectsMissingUtxoBeforeSignatureChecking)
{
    Scenario scenario;
    PopulateScenario(scenario);
    scenario.block.transactions.at(1).inputs.front().previous_output.transaction_id =
        HashWithLastByte(0x12U);
    ASSERT_TRUE(FinalizeBlock(scenario.block, scenario.parameters.block_limits.transaction_limits));
    EXPECT_EQ(
        ConnectBlock(scenario.block, scenario.context, scenario.parameters, scenario.utxos).error,
        BlockValidationError::kMissingUtxo);
}

TEST(BlockValidation, RejectsTimestampCoinbaseHeightAndNonfinalTransactions)
{
    Scenario old_time;
    PopulateScenario(old_time);
    old_time.block.header.time = old_time.context.median_time_past;
    ASSERT_TRUE(MineHeader(old_time.block.header));
    EXPECT_EQ(
        ConnectBlock(old_time.block, old_time.context, old_time.parameters, old_time.utxos).error,
        BlockValidationError::kTimeTooOld);

    Scenario wrong_coinbase_height;
    PopulateScenario(wrong_coinbase_height);
    wrong_coinbase_height.block.transactions.front().inputs.front().script_sig = {0x01U, 0x09U};
    ASSERT_TRUE(FinalizeBlock(wrong_coinbase_height.block,
                              wrong_coinbase_height.parameters.block_limits.transaction_limits));
    EXPECT_EQ(ConnectBlock(wrong_coinbase_height.block, wrong_coinbase_height.context,
                           wrong_coinbase_height.parameters, wrong_coinbase_height.utxos)
                  .error,
              BlockValidationError::kInvalidCoinbaseHeight);

    Scenario nonfinal;
    PopulateScenario(nonfinal);
    nonfinal.block.transactions.at(1).inputs.front().sequence = 0U;
    nonfinal.block.transactions.at(1).lock_time = nonfinal.context.height;
    ResignFirstSpend(nonfinal);
    EXPECT_EQ(
        ConnectBlock(nonfinal.block, nonfinal.context, nonfinal.parameters, nonfinal.utxos).error,
        BlockValidationError::kNonFinalTransaction);
}

TEST(BlockValidation, RejectsInvalidSignatureWithNoStateMutation)
{
    Scenario scenario;
    PopulateScenario(scenario);
    ResignFirstSpend(scenario, 2U);
    EXPECT_EQ(
        ConnectBlock(scenario.block, scenario.context, scenario.parameters, scenario.utxos).error,
        BlockValidationError::kInvalidSignature);
    EXPECT_TRUE(scenario.utxos.HaveCoin(scenario.spent_key));
}

TEST(BlockValidation, RejectsImmatureCoinbaseAndInsufficientInputs)
{
    Scenario immature;
    PopulateScenario(immature, true, 9U);
    EXPECT_EQ(
        ConnectBlock(immature.block, immature.context, immature.parameters, immature.utxos).error,
        BlockValidationError::kImmatureCoinbase);

    Scenario insufficient;
    PopulateScenario(insufficient);
    insufficient.block.transactions.at(1).outputs.front().value = 51LL * COIN;
    ResignFirstSpend(insufficient);
    EXPECT_EQ(ConnectBlock(insufficient.block, insufficient.context, insufficient.parameters,
                           insufficient.utxos)
                  .error,
              BlockValidationError::kInputValueTooLow);
}

TEST(BlockValidation, RejectsInflationAndCrossTransactionDoubleSpends)
{
    Scenario inflation;
    PopulateScenario(inflation);
    inflation.block.transactions.front().outputs.front().value = 26LL * COIN + 1LL;
    ASSERT_TRUE(
        FinalizeBlock(inflation.block, inflation.parameters.block_limits.transaction_limits));
    EXPECT_EQ(
        ConnectBlock(inflation.block, inflation.context, inflation.parameters, inflation.utxos)
            .error,
        BlockValidationError::kInvalidCoinbaseReward);

    Scenario double_spend;
    PopulateScenario(double_spend);
    auto second_spend = double_spend.block.transactions.at(1);
    second_spend.lock_time = 1U;
    const auto private_key = KeyFor(1U);
    ASSERT_TRUE(private_key.has_value());
    const auto public_key = private_key->DerivePublicKey();
    ASSERT_TRUE(public_key.has_value());
    ASSERT_TRUE(SignInput(second_spend, *private_key, *public_key, double_spend.locking_script,
                          double_spend.parameters.block_limits.transaction_limits));
    double_spend.block.transactions.push_back(std::move(second_spend));
    double_spend.block.transactions.front().outputs.front().value = 27LL * COIN;
    ASSERT_TRUE(
        FinalizeBlock(double_spend.block, double_spend.parameters.block_limits.transaction_limits));
    EXPECT_EQ(ConnectBlock(double_spend.block, double_spend.context, double_spend.parameters,
                           double_spend.utxos)
                  .error,
              BlockValidationError::kDoubleSpend);
}

} // namespace
