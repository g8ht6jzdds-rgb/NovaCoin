#include <gtest/gtest.h>

#include "chain/utxo.hpp"

#include <cstdint>
#include <vector>

namespace
{

using nova::chain::ApplyBlockResult;
using nova::chain::Coin;
using nova::chain::UTXOError;
using nova::chain::UTXOKey;
using nova::chain::UTXOSet;
using nova::primitives::OutPoint;
using nova::primitives::Transaction;
using nova::primitives::TransactionLimits;
using nova::primitives::TxInput;
using nova::primitives::TxOutput;

constexpr TransactionLimits kLimits{21'000'000LL * 100'000'000LL, 1'000'000U, 16U, 16U, 256U};

nova::crypto::Hash256 HashWithLastByte(const std::uint8_t value)
{
    nova::crypto::Hash256::Bytes bytes{};
    bytes.back() = value;
    return nova::crypto::Hash256{bytes};
}

UTXOKey Key(const std::uint8_t transaction_marker, const std::uint32_t output_index)
{
    return UTXOKey{HashWithLastByte(transaction_marker), output_index};
}

Coin CoinWithValue(const std::int64_t value)
{
    return Coin{TxOutput{value, {0x51U}}, 100U, false};
}

Transaction SpendingTransaction(const UTXOKey& input, const std::int64_t output_value)
{
    return Transaction{
        1,
        std::vector<TxInput>{
            TxInput{OutPoint{input.transaction_id, input.output_index}, {0x51U}, 0xFFFFFFFFU}},
        std::vector<TxOutput>{TxOutput{output_value, {0x51U}}}, 0U};
}

TEST(UTXOSetBasics, CreatesDeletesAndFindsCoins)
{
    UTXOSet set;
    const auto key = Key(1U, 0U);
    const auto coin = CoinWithValue(50);
    EXPECT_FALSE(set.HaveCoin(key));
    ASSERT_TRUE(set.AddCoin(key, coin));
    EXPECT_TRUE(set.HaveCoin(key));
    const auto fetched = set.GetCoin(key);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->output.value, 50);
    EXPECT_FALSE(set.AddCoin(key, coin));

    const auto spent = set.SpendCoin(key);
    ASSERT_TRUE(spent.has_value());
    EXPECT_EQ(spent->output.value, 50);
    EXPECT_FALSE(set.HaveCoin(key));
    EXPECT_FALSE(set.SpendCoin(key).has_value());
}

TEST(UTXOApply, SpendsInputsCreatesOutputsAndCanUndo)
{
    UTXOSet set;
    const auto input_key = Key(2U, 0U);
    ASSERT_TRUE(set.AddCoin(input_key, CoinWithValue(50)));
    const auto transaction = SpendingTransaction(input_key, 40);

    const auto applied = set.ApplyTransaction(transaction, 101U, false, kLimits);
    ASSERT_EQ(applied.error, UTXOError::kNone);
    ASSERT_TRUE(applied.undo.has_value());
    EXPECT_FALSE(set.HaveCoin(input_key));
    const auto transaction_id = transaction.TxId(kLimits);
    ASSERT_TRUE(transaction_id.has_value());
    const UTXOKey created{*transaction_id, 0U};
    EXPECT_TRUE(set.HaveCoin(created));
    EXPECT_FALSE(set.HaveCoin(UTXOKey{*transaction_id, 1U}));

    EXPECT_EQ(set.UndoTransaction(*applied.undo), UTXOError::kNone);
    EXPECT_TRUE(set.HaveCoin(input_key));
    EXPECT_FALSE(set.HaveCoin(created));
}

TEST(UTXOApply, DoesNotExposeOutOfRangeCreatedOutput)
{
    UTXOSet set;
    const auto input_key = Key(8U, 0U);
    ASSERT_TRUE(set.AddCoin(input_key, CoinWithValue(50)));
    const auto transaction = SpendingTransaction(input_key, 40);
    ASSERT_EQ(set.ApplyTransaction(transaction, 101U, false, kLimits).error, UTXOError::kNone);
    const auto transaction_id = transaction.TxId(kLimits);
    ASSERT_TRUE(transaction_id.has_value());

    EXPECT_TRUE(set.HaveCoin(UTXOKey{*transaction_id, 0U}));
    EXPECT_FALSE(set.HaveCoin(UTXOKey{*transaction_id, 1U}));
    EXPECT_FALSE(set.SpendCoin(UTXOKey{*transaction_id, 1U}).has_value());
}

TEST(UTXOApply, RejectsMissingDoubleAndDuplicateInputs)
{
    UTXOSet set;
    const auto missing_key = Key(3U, 0U);
    const auto missing_transaction = SpendingTransaction(missing_key, 40);
    EXPECT_EQ(set.ApplyTransaction(missing_transaction, 101U, false, kLimits).error,
              UTXOError::kMissingInput);
    EXPECT_EQ(set.size(), 0U);

    const auto input_key = Key(4U, 0U);
    ASSERT_TRUE(set.AddCoin(input_key, CoinWithValue(50)));
    const auto transaction = SpendingTransaction(input_key, 40);
    ASSERT_EQ(set.ApplyTransaction(transaction, 101U, false, kLimits).error, UTXOError::kNone);
    EXPECT_EQ(set.ApplyTransaction(transaction, 102U, false, kLimits).error,
              UTXOError::kMissingInput);

    UTXOSet duplicate_set;
    ASSERT_TRUE(duplicate_set.AddCoin(input_key, CoinWithValue(50)));
    auto duplicate_transaction = SpendingTransaction(input_key, 40);
    duplicate_transaction.inputs.push_back(duplicate_transaction.inputs.front());
    EXPECT_EQ(duplicate_set.ApplyTransaction(duplicate_transaction, 101U, false, kLimits).error,
              UTXOError::kInvalidTransactionStructure);
    EXPECT_TRUE(duplicate_set.HaveCoin(input_key));
}

TEST(UTXOApply, RollsBackMultiTransactionBlockOnFailure)
{
    UTXOSet set;
    const auto input_key = Key(5U, 0U);
    ASSERT_TRUE(set.AddCoin(input_key, CoinWithValue(50)));
    const auto first = SpendingTransaction(input_key, 40);
    const auto second = SpendingTransaction(Key(6U, 0U), 30);
    const std::vector<Transaction> block{first, second};

    const ApplyBlockResult result = set.ApplyBlock(block, 101U, kLimits);
    EXPECT_EQ(result.error, UTXOError::kMissingInput);
    EXPECT_FALSE(result.undo.has_value());
    EXPECT_TRUE(set.HaveCoin(input_key));
    const auto first_id = first.TxId(kLimits);
    ASSERT_TRUE(first_id.has_value());
    EXPECT_FALSE(set.HaveCoin(UTXOKey{*first_id, 0U}));
    EXPECT_EQ(set.size(), 1U);
}

TEST(UTXOApply, AppliesAndUndoesMultiTransactionBlock)
{
    UTXOSet set;
    const auto input_key = Key(7U, 0U);
    ASSERT_TRUE(set.AddCoin(input_key, CoinWithValue(50)));
    const auto first = SpendingTransaction(input_key, 40);
    const auto first_id = first.TxId(kLimits);
    ASSERT_TRUE(first_id.has_value());
    const auto second = SpendingTransaction(UTXOKey{*first_id, 0U}, 30);
    const std::vector<Transaction> block{first, second};

    const auto applied = set.ApplyBlock(block, 101U, kLimits);
    ASSERT_EQ(applied.error, UTXOError::kNone);
    ASSERT_TRUE(applied.undo.has_value());
    const auto second_id = second.TxId(kLimits);
    ASSERT_TRUE(second_id.has_value());
    EXPECT_FALSE(set.HaveCoin(input_key));
    EXPECT_FALSE(set.HaveCoin(UTXOKey{*first_id, 0U}));
    EXPECT_TRUE(set.HaveCoin(UTXOKey{*second_id, 0U}));

    EXPECT_EQ(set.UndoBlock(*applied.undo), UTXOError::kNone);
    EXPECT_TRUE(set.HaveCoin(input_key));
    EXPECT_FALSE(set.HaveCoin(UTXOKey{*second_id, 0U}));
}

TEST(UTXOBatchAtomicity, UsesABoundedOverlayAndRollbackDoesNotTouchTheBaseSet)
{
    UTXOSet set;
    constexpr std::uint8_t kBaseCoins = 64U;
    for (std::uint8_t marker = 1U; marker <= kBaseCoins; ++marker) {
        ASSERT_TRUE(set.AddCoin(Key(marker, 0U), CoinWithValue(50)));
    }
    const auto initial_size = set.size();
    auto batch = set.BeginBatch();
    ASSERT_TRUE(batch.has_value());
    const auto transaction = SpendingTransaction(Key(1U, 0U), 40);
    const auto applied = batch->ApplyTransaction(transaction, 101U, false, kLimits);
    ASSERT_EQ(applied.error, UTXOError::kNone);
    // The batch tracks the one spend and one new output, not a copy of the
    // sixty-four untouched base entries.
    EXPECT_EQ(batch->overlay_entry_count(), 2U);
    batch->Rollback();

    EXPECT_EQ(set.size(), initial_size);
    EXPECT_TRUE(set.HaveCoin(Key(1U, 0U)));
    const auto transaction_id = transaction.TxId(kLimits);
    ASSERT_TRUE(transaction_id.has_value());
    EXPECT_FALSE(set.HaveCoin(UTXOKey{*transaction_id, 0U}));
}

} // namespace
