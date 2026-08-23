#include <gtest/gtest.h>

#include "chain/mempool.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace
{

using nova::chain::BlockValidationParams;
using nova::chain::Mempool;
using nova::chain::MempoolError;
using nova::chain::MempoolParams;
using nova::chain::TransactionValidationContext;
using nova::chain::UTXOKey;
using nova::chain::UTXOSet;
using nova::consensus::ChainParams;
using nova::consensus::COIN;
using nova::consensus::INITIAL_SUBSIDY;
using nova::consensus::MAX_MONEY;
using nova::primitives::Amount;
using nova::primitives::BlockLimits;
using nova::primitives::OutPoint;
using nova::primitives::Transaction;
using nova::primitives::TransactionLimits;
using nova::primitives::TxInput;
using nova::primitives::TxOutput;

struct MempoolScenario final {
    MempoolParams parameters;
    TransactionValidationContext context{10U, 1'000U};
    UTXOSet utxos;
    std::optional<nova::crypto::PrivateKey> private_key;
    std::optional<nova::crypto::PublicKey> public_key;
    std::vector<std::uint8_t> locking_script;
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

void PopulateScenario(MempoolScenario& scenario)
{
    constexpr TransactionLimits kTransactionLimits{MAX_MONEY, 100'000U, 8U, 8U, 128U};
    constexpr BlockLimits kBlockLimits{kTransactionLimits, 100'000U, 8U, 64U};
    scenario.parameters =
        MempoolParams{BlockValidationParams{kBlockLimits, nova::consensus::RegtestPowParameters(),
                                            ChainParams{COIN, MAX_MONEY, INITIAL_SUBSIDY, 10U}, 0U,
                                            500'000'000U, 0xFFFF'FFFFU, 1U, 600U},
                      64U, 1'000'000U, 0};
    auto private_key = KeyFor(1U);
    ASSERT_TRUE(private_key.has_value());
    scenario.private_key = std::move(*private_key);
    const auto public_key = scenario.private_key->DerivePublicKey();
    ASSERT_TRUE(public_key.has_value());
    scenario.public_key = *public_key;
    scenario.locking_script = P2pkhScript(*scenario.public_key);
}

UTXOKey AddSpendableCoin(MempoolScenario& scenario, const std::uint8_t identifier,
                         const Amount value = 50LL * COIN)
{
    const UTXOKey key{HashWithLastByte(identifier), 0U};
    EXPECT_TRUE(scenario.utxos.AddCoin(
        key, nova::chain::Coin{TxOutput{value, scenario.locking_script}, 1U, false}));
    return key;
}

std::optional<Transaction> MakeSpend(MempoolScenario& scenario, const UTXOKey& input,
                                     const Amount output_value)
{
    Transaction transaction{
        1,
        std::vector<TxInput>{
            TxInput{OutPoint{input.transaction_id, input.output_index}, {}, 0xFFFF'FFFFU}},
        std::vector<TxOutput>{TxOutput{output_value, scenario.locking_script}}, 0U};
    if (!SignInput(transaction, *scenario.private_key, *scenario.public_key,
                   scenario.locking_script,
                   scenario.parameters.validation.block_limits.transaction_limits)) {
        return std::nullopt;
    }
    return transaction;
}

std::unique_ptr<Mempool> CreateMempool(const MempoolScenario& scenario)
{
    return Mempool::Create(scenario.parameters);
}

nova::crypto::Hash256 TransactionId(const Transaction& transaction, const MempoolScenario& scenario)
{
    const auto transaction_id =
        transaction.TxId(scenario.parameters.validation.block_limits.transaction_limits);
    EXPECT_TRUE(transaction_id.has_value());
    return transaction_id.value_or(nova::crypto::Hash256{});
}

TEST(Mempool, AcceptsValidTransactionsAndTracksCheckedMetadata)
{
    MempoolScenario scenario;
    PopulateScenario(scenario);
    auto mempool = CreateMempool(scenario);
    ASSERT_NE(mempool, nullptr);
    const auto spend = MakeSpend(scenario, AddSpendableCoin(scenario, 0x11U), 49LL * COIN);
    ASSERT_TRUE(spend.has_value());

    const auto accepted = mempool->AcceptToMempool(*spend, scenario.context, scenario.utxos, 123U);
    ASSERT_EQ(accepted.error, MempoolError::kNone);
    ASSERT_TRUE(accepted.transaction_id.has_value());
    const auto entry = mempool->GetEntry(*accepted.transaction_id);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->fee, COIN);
    EXPECT_EQ(entry->time_received, 123U);
    EXPECT_TRUE(entry->serialized_size > 0U);
    EXPECT_EQ(entry->fee_rate, entry->fee / static_cast<Amount>(entry->serialized_size));
    EXPECT_TRUE(entry->dependencies.empty());
}

TEST(Mempool, RejectsMissingInputsOversizedTransactionsAndDoubleSpends)
{
    MempoolScenario scenario;
    PopulateScenario(scenario);
    auto mempool = CreateMempool(scenario);
    ASSERT_NE(mempool, nullptr);
    const auto missing = MakeSpend(scenario, UTXOKey{HashWithLastByte(0x22U), 0U}, 49LL * COIN);
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(mempool->AcceptToMempool(*missing, scenario.context, scenario.utxos, 1U).error,
              MempoolError::kMissingInput);

    const auto input = AddSpendableCoin(scenario, 0x23U);
    const auto first = MakeSpend(scenario, input, 49LL * COIN);
    const auto second = MakeSpend(scenario, input, 48LL * COIN);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(mempool->AcceptToMempool(*first, scenario.context, scenario.utxos, 2U).error,
              MempoolError::kNone);
    EXPECT_EQ(mempool->AcceptToMempool(*second, scenario.context, scenario.utxos, 3U).error,
              MempoolError::kMempoolDoubleSpend);

    const auto oversized = MakeSpend(scenario, AddSpendableCoin(scenario, 0x24U), 49LL * COIN);
    ASSERT_TRUE(oversized.has_value());
    auto malformed = *oversized;
    malformed.outputs.front().script_pubkey.assign(129U, 0x51U);
    EXPECT_EQ(mempool->AcceptToMempool(malformed, scenario.context, scenario.utxos, 4U).error,
              MempoolError::kInvalidTransaction);

    const auto signed_transaction =
        MakeSpend(scenario, AddSpendableCoin(scenario, 0x25U), 49LL * COIN);
    ASSERT_TRUE(signed_transaction.has_value());
    auto bad_signature = *signed_transaction;
    bad_signature.outputs.front().value = 48LL * COIN;
    EXPECT_EQ(mempool->AcceptToMempool(bad_signature, scenario.context, scenario.utxos, 5U).error,
              MempoolError::kInvalidTransaction);

    const auto inflation = MakeSpend(scenario, AddSpendableCoin(scenario, 0x26U), 51LL * COIN);
    ASSERT_TRUE(inflation.has_value());
    const auto inflation_result =
        mempool->AcceptToMempool(*inflation, scenario.context, scenario.utxos, 6U);
    EXPECT_EQ(inflation_result.error, MempoolError::kInvalidTransaction);
    EXPECT_EQ(inflation_result.validation_error,
              nova::chain::BlockValidationError::kInputValueTooLow);
}

TEST(Mempool, TracksDependenciesAndSelectsParentsBeforeChildren)
{
    MempoolScenario scenario;
    PopulateScenario(scenario);
    auto mempool = CreateMempool(scenario);
    ASSERT_NE(mempool, nullptr);
    const auto parent = MakeSpend(scenario, AddSpendableCoin(scenario, 0x31U), 49LL * COIN);
    ASSERT_TRUE(parent.has_value());
    ASSERT_EQ(mempool->AcceptToMempool(*parent, scenario.context, scenario.utxos, 1U).error,
              MempoolError::kNone);
    const auto parent_id = TransactionId(*parent, scenario);
    const auto child = MakeSpend(scenario, UTXOKey{parent_id, 0U}, 48LL * COIN);
    ASSERT_TRUE(child.has_value());
    ASSERT_EQ(mempool->AcceptToMempool(*child, scenario.context, scenario.utxos, 2U).error,
              MempoolError::kNone);
    const auto child_id = TransactionId(*child, scenario);
    const auto child_entry = mempool->GetEntry(child_id);
    ASSERT_TRUE(child_entry.has_value());
    ASSERT_EQ(child_entry->dependencies.size(), 1U);
    EXPECT_EQ(child_entry->dependencies.front(), parent_id);

    const auto selected = mempool->SelectForBlock(1'000'000U);
    ASSERT_EQ(selected.error, MempoolError::kNone);
    ASSERT_EQ(selected.transactions.size(), 2U);
    EXPECT_EQ(TransactionId(selected.transactions.at(0), scenario), parent_id);
    EXPECT_EQ(TransactionId(selected.transactions.at(1), scenario), child_id);
}

TEST(Mempool, RemovesDescendantsAndRevalidatesChildrenAfterConfirmation)
{
    MempoolScenario scenario;
    PopulateScenario(scenario);
    auto mempool = CreateMempool(scenario);
    ASSERT_NE(mempool, nullptr);
    const auto parent_input = AddSpendableCoin(scenario, 0x41U);
    const auto parent = MakeSpend(scenario, parent_input, 49LL * COIN);
    ASSERT_TRUE(parent.has_value());
    ASSERT_EQ(mempool->AcceptToMempool(*parent, scenario.context, scenario.utxos, 1U).error,
              MempoolError::kNone);
    const auto parent_id = TransactionId(*parent, scenario);
    const auto child = MakeSpend(scenario, UTXOKey{parent_id, 0U}, 48LL * COIN);
    ASSERT_TRUE(child.has_value());
    ASSERT_EQ(mempool->AcceptToMempool(*child, scenario.context, scenario.utxos, 2U).error,
              MempoolError::kNone);
    EXPECT_EQ(mempool->RemoveTransaction(parent_id).removed_count, 2U);
    EXPECT_EQ(mempool->size(), 0U);

    ASSERT_EQ(mempool->AcceptToMempool(*parent, scenario.context, scenario.utxos, 3U).error,
              MempoolError::kNone);
    ASSERT_EQ(mempool->AcceptToMempool(*child, scenario.context, scenario.utxos, 4U).error,
              MempoolError::kNone);
    const std::vector<Transaction> confirmed{*parent};
    EXPECT_EQ(mempool->RemoveForBlock(confirmed).removed_count, 1U);
    ASSERT_EQ(scenario.utxos
                  .ApplyTransaction(*parent, scenario.context.height, false,
                                    scenario.parameters.validation.block_limits.transaction_limits)
                  .error,
              nova::chain::UTXOError::kNone);
    EXPECT_EQ(mempool->RevalidateMempool(scenario.context, scenario.utxos).error,
              MempoolError::kNone);
    EXPECT_EQ(mempool->size(), 1U);
    const auto child_entry = mempool->GetEntry(TransactionId(*child, scenario));
    ASSERT_TRUE(child_entry.has_value());
    EXPECT_TRUE(child_entry->dependencies.empty());
}

TEST(Mempool, RevalidationDropsTransactionsWhoseInputsLeaveTheActiveUtxoSet)
{
    MempoolScenario scenario;
    PopulateScenario(scenario);
    auto mempool = CreateMempool(scenario);
    ASSERT_NE(mempool, nullptr);
    const auto input = AddSpendableCoin(scenario, 0x51U);
    const auto spend = MakeSpend(scenario, input, 49LL * COIN);
    ASSERT_TRUE(spend.has_value());
    ASSERT_EQ(mempool->AcceptToMempool(*spend, scenario.context, scenario.utxos, 1U).error,
              MempoolError::kNone);
    ASSERT_TRUE(scenario.utxos.SpendCoin(input).has_value());

    const auto revalidated = mempool->RevalidateMempool(scenario.context, scenario.utxos);
    EXPECT_EQ(revalidated.error, MempoolError::kNone);
    EXPECT_EQ(revalidated.removed_count, 1U);
    EXPECT_EQ(mempool->size(), 0U);
}

TEST(Mempool, HandlesDeterministicStressAdmissionAndFeeRateSelection)
{
    MempoolScenario scenario;
    PopulateScenario(scenario);
    auto mempool = CreateMempool(scenario);
    ASSERT_NE(mempool, nullptr);
    std::optional<nova::crypto::Hash256> highest_fee_id;
    for (std::uint8_t identifier = 1U; identifier <= 32U; ++identifier) {
        const auto spend = MakeSpend(scenario, AddSpendableCoin(scenario, identifier),
                                     50LL * COIN - static_cast<Amount>(identifier));
        ASSERT_TRUE(spend.has_value());
        const auto accepted = mempool->AcceptToMempool(*spend, scenario.context, scenario.utxos,
                                                       static_cast<std::uint64_t>(identifier));
        ASSERT_EQ(accepted.error, MempoolError::kNone);
        if (identifier == 32U) {
            highest_fee_id = accepted.transaction_id;
        }
    }
    ASSERT_EQ(mempool->size(), 32U);
    ASSERT_TRUE(highest_fee_id.has_value());
    const auto selected = mempool->SelectForBlock(1'000'000U);
    ASSERT_EQ(selected.error, MempoolError::kNone);
    ASSERT_EQ(selected.transactions.size(), 32U);
    EXPECT_EQ(TransactionId(selected.transactions.front(), scenario), *highest_fee_id);
}

} // namespace
