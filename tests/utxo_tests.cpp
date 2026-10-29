#include <gtest/gtest.h>

#include "chain/utxo.hpp"

#include <cstdint>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

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

TEST(UTXONestedBatch, ChildCommitIsPrivateUntilOuterCommit)
{
    UTXOSet base;
    const auto original = Key(90U, 0U);
    ASSERT_TRUE(base.AddCoin(original, CoinWithValue(50)));
    auto outer = base.BeginBatch();
    ASSERT_TRUE(outer.has_value());
    auto child = outer->BeginBatch();
    ASSERT_TRUE(child.has_value());
    const auto first = SpendingTransaction(original, 40);
    const auto first_key = UTXOKey{first.TxId(kLimits).value(), 0U};
    const auto applied = child->ApplyTransaction(first, 101U, false, kLimits);
    ASSERT_EQ(applied.error, UTXOError::kNone);
    ASSERT_TRUE(child->Commit());
    EXPECT_TRUE(base.HaveCoin(original));
    EXPECT_FALSE(base.HaveCoin(first_key));
    EXPECT_FALSE(outer->HaveCoin(original));
    EXPECT_TRUE(outer->HaveCoin(first_key));
    auto next = outer->BeginBatch();
    ASSERT_TRUE(next.has_value());
    const auto second = SpendingTransaction(first_key, 30);
    const auto last_key = UTXOKey{second.TxId(kLimits).value(), 0U};
    ASSERT_EQ(next->ApplyTransaction(second, 102U, false, kLimits).error, UTXOError::kNone);
    ASSERT_TRUE(next->Commit());
    EXPECT_FALSE(outer->HaveCoin(first_key));
    EXPECT_EQ(outer->overlay_entry_count(), 2U);
    ASSERT_TRUE(outer->Commit());
    EXPECT_EQ(base.size(), 1U);
    EXPECT_FALSE(base.HaveCoin(original));
    EXPECT_FALSE(base.HaveCoin(first_key));
    const auto final = base.GetCoin(last_key);
    ASSERT_TRUE(final.has_value());
    EXPECT_EQ(final->output.value, 30);
    EXPECT_EQ(final->height, 102U);
    EXPECT_FALSE(outer->Commit());
}

TEST(UTXONestedBatch, AbortUndoAndStaleChildrenDoNotPublishPartialState)
{
    UTXOSet base;
    const auto original = Key(91U, 0U);
    ASSERT_TRUE(base.AddCoin(original, CoinWithValue(50)));
    const auto transaction = SpendingTransaction(original, 40);
    const auto created = UTXOKey{transaction.TxId(kLimits).value(), 0U};
    auto outer = base.BeginBatch();
    ASSERT_TRUE(outer.has_value());
    auto child = outer->BeginBatch();
    auto stale = outer->BeginBatch();
    ASSERT_TRUE(child.has_value());
    ASSERT_TRUE(stale.has_value());
    const auto applied = child->ApplyTransaction(transaction, 101U, false, kLimits);
    ASSERT_EQ(applied.error, UTXOError::kNone);
    ASSERT_TRUE(applied.undo.has_value());
    ASSERT_TRUE(child->Commit());
    EXPECT_FALSE(stale->Commit());
    EXPECT_EQ(stale->ApplyTransaction(transaction, 101U, false, kLimits).error,
              UTXOError::kBatchClosed);
    auto undo = outer->BeginBatch();
    ASSERT_TRUE(undo.has_value());
    ASSERT_EQ(undo->UndoTransaction(*applied.undo), UTXOError::kNone);
    ASSERT_TRUE(undo->Commit());
    EXPECT_TRUE(outer->HaveCoin(original));
    EXPECT_FALSE(outer->HaveCoin(created));
    ASSERT_TRUE(outer->Commit());
    EXPECT_EQ(base.size(), 1U);
    const auto restored = base.GetCoin(original);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->output.value, 50);
    EXPECT_EQ(restored->height, 100U);
    auto aborted = base.BeginBatch();
    ASSERT_TRUE(aborted.has_value());
    auto nested = aborted->BeginBatch();
    ASSERT_TRUE(nested.has_value());
    ASSERT_EQ(nested->ApplyTransaction(transaction, 101U, false, kLimits).error, UTXOError::kNone);
    aborted->Rollback();
    EXPECT_FALSE(nested->Commit());
    EXPECT_FALSE(aborted->BeginBatch().has_value());
    EXPECT_TRUE(base.HaveCoin(original));
    EXPECT_FALSE(base.HaveCoin(created));
}

TEST(UTXONestedBatch, SpendingBaseOutpointDoesNotPermitInBlockCollision)
{
    UTXOSet base;
    const auto original = Key(92U, 0U);
    ASSERT_TRUE(base.AddCoin(original, CoinWithValue(50)));
    const auto creates_collision = SpendingTransaction(original, 40);
    const auto collision = UTXOKey{creates_collision.TxId(kLimits).value(), 0U};
    ASSERT_TRUE(base.AddCoin(collision, CoinWithValue(10)));
    auto parent = base.BeginBatch();
    ASSERT_TRUE(parent.has_value());
    auto child = parent->BeginBatch();
    ASSERT_TRUE(child.has_value());
    const auto spends_collision = SpendingTransaction(collision, 9);
    ASSERT_EQ(child->ApplyTransaction(spends_collision, 101U, false, kLimits).error,
              UTXOError::kNone);
    const auto entries = child->overlay_entry_count();
    EXPECT_EQ(child->ApplyTransaction(creates_collision, 101U, false, kLimits).error,
              UTXOError::kDuplicateCoin);
    EXPECT_EQ(child->overlay_entry_count(), entries);
    EXPECT_TRUE(child->HaveCoin(original));
    EXPECT_FALSE(child->HaveCoin(collision));
    child->Rollback();
    EXPECT_EQ(parent->overlay_entry_count(), 0U);
    ASSERT_TRUE(parent->Commit());
    EXPECT_EQ(base.size(), 2U);
    EXPECT_TRUE(base.HaveCoin(original));
    EXPECT_TRUE(base.HaveCoin(collision));
}

TEST(UTXONestedBatch, MultilevelCompositionMatchesFlatApplyUndoAndAbort)
{
    for (const bool commit : {false, true}) {
        UTXOSet published;
        UTXOSet reference;
        std::vector<UTXOKey> keys;
        for (const auto marker : {100U, 101U, 102U, 103U}) {
            const auto key = Key(static_cast<std::uint8_t>(marker), 0U);
            ASSERT_TRUE(published.AddCoin(key, CoinWithValue(50)));
            ASSERT_TRUE(reference.AddCoin(key, CoinWithValue(50)));
            keys.push_back(key);
        }
        auto outer = published.BeginBatch();
        ASSERT_TRUE(outer.has_value());
        std::vector<nova::chain::TransactionUndo> undo_stack;
        const auto compare_view = [&]() {
            for (const auto& key : keys) {
                const auto expected = reference.GetCoin(key);
                const auto actual = outer->GetCoin(key);
                ASSERT_EQ(actual.has_value(), expected.has_value());
                EXPECT_EQ(outer->HaveCoin(key), reference.HaveCoin(key));
                if (actual.has_value()) {
                    EXPECT_EQ(actual->output.value, expected->output.value);
                    EXPECT_EQ(actual->output.script_pubkey, expected->output.script_pubkey);
                    EXPECT_EQ(actual->height, expected->height);
                    EXPECT_EQ(actual->is_coinbase, expected->is_coinbase);
                }
            }
        };
        for (unsigned step = 0U; step < 24U; ++step) {
            SCOPED_TRACE(step);
            auto child = outer->BeginBatch();
            ASSERT_TRUE(child.has_value());
            auto nested = child->BeginBatch();
            ASSERT_TRUE(nested.has_value());
            if (step % 3U == 2U && !undo_stack.empty()) {
                const nova::chain::BlockUndo undo{{undo_stack.back()}};
                ASSERT_EQ(reference.UndoBlock(undo), UTXOError::kNone);
                ASSERT_EQ(nested->UndoBlock(undo), UTXOError::kNone);
                undo_stack.pop_back();
            } else {
                UTXOKey input = Key(255U, 0U); // deliberately missing every fifth step
                if (step % 5U != 0U) {
                    bool found = false;
                    for (const auto& key : keys) {
                        if (reference.HaveCoin(key)) {
                            input = key;
                            found = true;
                            break;
                        }
                    }
                    ASSERT_TRUE(found);
                }
                const auto coin = reference.GetCoin(input);
                const auto value = coin.has_value() ? coin->output.value - 1 : 1;
                const auto transaction = SpendingTransaction(input, value);
                keys.push_back({transaction.TxId(kLimits).value(), 0U});
                const auto expected =
                    reference.ApplyTransaction(transaction, 101U + step, false, kLimits);
                const auto actual =
                    nested->ApplyTransaction(transaction, 101U + step, false, kLimits);
                ASSERT_EQ(actual.error, expected.error);
                if (expected.error != UTXOError::kNone) {
                    EXPECT_EQ(expected.error, UTXOError::kMissingInput);
                    nested->Rollback();
                    child->Rollback();
                    compare_view();
                    continue;
                }
                ASSERT_TRUE(expected.undo.has_value());
                undo_stack.push_back(*expected.undo);
            }
            ASSERT_TRUE(nested->Commit());
            ASSERT_TRUE(child->Commit());
            compare_view();
            EXPECT_EQ(published.size(), 4U);
            for (std::size_t i = 0U; i < 4U; ++i) {
                const auto original = published.GetCoin(keys[i]);
                ASSERT_TRUE(original.has_value());
                EXPECT_EQ(original->output.value, 50);
            }
        }
        if (commit) {
            ASSERT_TRUE(outer->Commit());
            EXPECT_EQ(published.size(), reference.size());
            for (const auto& key : keys) {
                const auto actual = published.GetCoin(key);
                const auto expected = reference.GetCoin(key);
                ASSERT_EQ(actual.has_value(), expected.has_value());
                if (actual.has_value()) {
                    EXPECT_EQ(actual->output.value, expected->output.value);
                    EXPECT_EQ(actual->output.script_pubkey, expected->output.script_pubkey);
                    EXPECT_EQ(actual->height, expected->height);
                    EXPECT_EQ(actual->is_coinbase, expected->is_coinbase);
                }
            }
        } else {
            outer->Rollback();
            EXPECT_EQ(published.size(), 4U);
            for (std::size_t i = 0U; i < 4U; ++i)
                EXPECT_TRUE(published.HaveCoin(keys[i]));
        }
    }
}

TEST(UTXOBatchGeneration, ReplacementOfLiveCoinInvalidatesStagedSpend)
{
    UTXOSet base;
    const auto input = Key(93U, 0U);
    ASSERT_TRUE(base.AddCoin(input, CoinWithValue(50)));
    auto staged = base.BeginBatch();
    ASSERT_TRUE(staged.has_value());
    const auto transaction = SpendingTransaction(input, 40);
    const auto created = UTXOKey{transaction.TxId(kLimits).value(), 0U};
    ASSERT_EQ(staged->ApplyTransaction(transaction, 101U, false, kLimits).error, UTXOError::kNone);
    auto nested = staged->BeginBatch();
    ASSERT_TRUE(nested.has_value());
    ASSERT_TRUE(base.SpendCoin(input).has_value());
    ASSERT_TRUE(base.AddCoin(input, CoinWithValue(1)));
    EXPECT_FALSE(staged->Commit());
    EXPECT_FALSE(nested->Commit());
    EXPECT_FALSE(staged->BeginBatch().has_value());
    EXPECT_FALSE(base.HaveCoin(created));
    ASSERT_EQ(base.size(), 1U);
    const auto replacement = base.GetCoin(input);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_EQ(replacement->output.value, 1);
}

TEST(UTXOBatchGeneration, SiblingCommitInvalidatesOtherOpenBatches)
{
    UTXOSet base;
    const auto first_key = Key(94U, 0U);
    const auto second_key = Key(95U, 0U);
    ASSERT_TRUE(base.AddCoin(first_key, CoinWithValue(50)));
    ASSERT_TRUE(base.AddCoin(second_key, CoinWithValue(50)));
    auto first = base.BeginBatch();
    auto stale = base.BeginBatch();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(stale.has_value());
    const auto first_tx = SpendingTransaction(first_key, 40);
    const auto stale_tx = SpendingTransaction(second_key, 40);
    ASSERT_EQ(first->ApplyTransaction(first_tx, 101U, false, kLimits).error, UTXOError::kNone);
    ASSERT_EQ(stale->ApplyTransaction(stale_tx, 101U, false, kLimits).error, UTXOError::kNone);
    ASSERT_TRUE(first->Commit());
    EXPECT_FALSE(stale->Commit());
    EXPECT_EQ(stale->ApplyTransaction(stale_tx, 102U, false, kLimits).error,
              UTXOError::kBatchClosed);
    EXPECT_TRUE(base.HaveCoin(second_key));
    EXPECT_FALSE(base.HaveCoin({stale_tx.TxId(kLimits).value(), 0U}));
}

TEST(UTXOBatchGeneration, AssignmentAndMoveInvalidatePriorOwners)
{
    for (const bool move : {false, true}) {
        UTXOSet base;
        UTXOSet replacement;
        const auto input = Key(96U, 0U);
        ASSERT_TRUE(base.AddCoin(input, CoinWithValue(50)));
        ASSERT_TRUE(replacement.AddCoin(input, CoinWithValue(1)));
        auto stale = base.BeginBatch();
        ASSERT_TRUE(stale.has_value());
        const auto transaction = SpendingTransaction(input, 40);
        ASSERT_EQ(stale->ApplyTransaction(transaction, 101U, false, kLimits).error,
                  UTXOError::kNone);
        if (move)
            base = std::move(replacement);
        else
            base = replacement;
        EXPECT_FALSE(stale->Commit());
        const auto actual = base.GetCoin(input);
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(actual->output.value, 1);
        EXPECT_FALSE(base.HaveCoin({transaction.TxId(kLimits).value(), 0U}));
    }
    UTXOSet source;
    const auto key = Key(97U, 0U);
    ASSERT_TRUE(source.AddCoin(key, CoinWithValue(50)));
    auto stale_source = source.BeginBatch();
    ASSERT_TRUE(stale_source.has_value());
    const auto transaction = SpendingTransaction(key, 40);
    ASSERT_EQ(stale_source->ApplyTransaction(transaction, 101U, false, kLimits).error,
              UTXOError::kNone);
    UTXOSet moved{std::move(source)};
    ASSERT_TRUE(source.AddCoin(key, CoinWithValue(1)));
    EXPECT_FALSE(stale_source->Commit());
    EXPECT_TRUE(source.HaveCoin(key));
    const auto moved_coin = moved.GetCoin(key);
    ASSERT_TRUE(moved_coin.has_value());
    EXPECT_EQ(moved_coin->output.value, 50);
}

TEST(UTXOBatchGeneration, NoChangeAndMovedFromBatchesCannotInvalidateNewOwner)
{
    UTXOSet base;
    const auto input = Key(98U, 0U);
    ASSERT_TRUE(base.AddCoin(input, CoinWithValue(50)));
    auto batch = base.BeginBatch();
    ASSERT_TRUE(batch.has_value());
    const auto transaction = SpendingTransaction(input, 40);
    const auto created = UTXOKey{transaction.TxId(kLimits).value(), 0U};
    ASSERT_EQ(batch->ApplyTransaction(transaction, 101U, false, kLimits).error, UTXOError::kNone);
    EXPECT_FALSE(base.AddCoin(input, CoinWithValue(1)));
    EXPECT_FALSE(base.SpendCoin(Key(99U, 0U)).has_value());
    UTXOSet copy{base};
    EXPECT_TRUE(copy.HaveCoin(input));
    nova::chain::UTXOBatch owner{std::move(*batch)};
    EXPECT_FALSE(batch->Commit());
    EXPECT_FALSE(batch->BeginBatch().has_value());
    ASSERT_TRUE(owner.Commit());
    EXPECT_FALSE(base.HaveCoin(input));
    EXPECT_TRUE(base.HaveCoin(created));
    EXPECT_TRUE(copy.HaveCoin(input));
    EXPECT_FALSE(copy.HaveCoin(created));
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

TEST(UTXOApply, RejectsExistingOutpointCollisionAtomically)
{
    UTXOSet set;
    const auto input_key = Key(9U, 0U);
    ASSERT_TRUE(set.AddCoin(input_key, CoinWithValue(50)));
    const auto transaction = SpendingTransaction(input_key, 40);
    const auto transaction_id = transaction.TxId(kLimits);
    ASSERT_TRUE(transaction_id.has_value());
    const UTXOKey collision{*transaction_id, 0U};
    ASSERT_TRUE(set.AddCoin(collision, CoinWithValue(7)));

    const auto applied = set.ApplyTransaction(transaction, 101U, false, kLimits);
    EXPECT_EQ(applied.error, UTXOError::kDuplicateCoin);
    EXPECT_FALSE(applied.undo.has_value());
    EXPECT_TRUE(set.HaveCoin(input_key));
    EXPECT_TRUE(set.HaveCoin(collision));
    EXPECT_EQ(set.size(), 2U);
}

TEST(UTXOApply, RejectsStagedAdditionCollisionAndRecoversDistinctOutputs)
{
    UTXOSet set;
    const Transaction coinbase{
        1,
        std::vector<TxInput>{
            TxInput{OutPoint{nova::crypto::Hash256{}, 0xFFFF'FFFFU}, {0x01U, 0x00U}, 0U}},
        std::vector<TxOutput>{{40, {0x51U}}, {9, {0x51U}}}, 0U};
    const auto transaction_id = coinbase.TxId(kLimits);
    ASSERT_TRUE(transaction_id.has_value());

    auto batch = set.BeginBatch();
    ASSERT_TRUE(batch.has_value());
    const auto first = batch->ApplyTransaction(coinbase, 101U, true, kLimits);
    ASSERT_EQ(first.error, UTXOError::kNone);
    ASSERT_TRUE(first.undo.has_value());
    EXPECT_TRUE(batch->HaveCoin(UTXOKey{*transaction_id, 0U}));
    EXPECT_TRUE(batch->HaveCoin(UTXOKey{*transaction_id, 1U}));

    const auto collision = batch->ApplyTransaction(coinbase, 101U, true, kLimits);
    EXPECT_EQ(collision.error, UTXOError::kDuplicateCoin);
    EXPECT_FALSE(collision.undo.has_value());
    EXPECT_TRUE(batch->Commit());
    EXPECT_TRUE(set.HaveCoin(UTXOKey{*transaction_id, 0U}));
    EXPECT_TRUE(set.HaveCoin(UTXOKey{*transaction_id, 1U}));

    EXPECT_EQ(set.UndoTransaction(*first.undo), UTXOError::kNone);
    EXPECT_FALSE(set.HaveCoin(UTXOKey{*transaction_id, 0U}));
    EXPECT_FALSE(set.HaveCoin(UTXOKey{*transaction_id, 1U}));
    EXPECT_EQ(set.size(), 0U);
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

#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool count_root_publication_allocations = false;
thread_local std::size_t root_publication_allocations = 0U;

int CountRootPublicationAllocations(const int kind, void*, std::size_t, int, long,
                                    const unsigned char*, int)
{
    if (count_root_publication_allocations && kind == _HOOK_ALLOC) {
        ++root_publication_allocations;
    }
    return 1; // Observe allocations; never force failure or touch heap data.
}
#endif

TEST(UTXOPublication, RootCommitUsesPreallocatedNodes)
{
    UTXOSet base;
    const auto original = Key(220U, 0U);
    const auto untouched = Key(221U, 0U);
    ASSERT_TRUE(base.AddCoin(original, CoinWithValue(50)));
    ASSERT_TRUE(base.AddCoin(untouched, CoinWithValue(70)));
    auto batch = base.BeginBatch();
    ASSERT_TRUE(batch.has_value());
    const auto transaction = SpendingTransaction(original, 40);
    const auto txid = transaction.TxId(kLimits);
    ASSERT_TRUE(txid.has_value());
    ASSERT_EQ(batch->ApplyTransaction(transaction, 101U, false, kLimits).error, UTXOError::kNone);
#if defined(_MSC_VER) && defined(_DEBUG)
    // Only the synchronous Commit call is measured. No GTest assertion,
    // logging, fixture construction or deliberate OOM occurs under this hook.
    // This fixture owns the process-wide hook for this single-thread test.
    const auto previous = _CrtSetAllocHook(CountRootPublicationAllocations);
    root_publication_allocations = 0U;
    count_root_publication_allocations = true;
#endif
    const auto committed = batch->Commit();
#if defined(_MSC_VER) && defined(_DEBUG)
    count_root_publication_allocations = false;
    _CrtSetAllocHook(previous);
    EXPECT_EQ(root_publication_allocations, 0U);
#endif
    ASSERT_TRUE(committed);
    EXPECT_EQ(base.size(), 2U);
    EXPECT_FALSE(base.HaveCoin(original));
    const auto created = base.GetCoin(UTXOKey{*txid, 0U});
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->output.value, 40);
    EXPECT_EQ(created->height, 101U);
    const auto retained = base.GetCoin(untouched);
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->output.value, 70);
    EXPECT_FALSE(batch->Commit());
}

TEST(UTXOReadOnly, OverlayAndDescendantsCannotPublishEvenAfterMove)
{
    UTXOSet base;
    const auto original = Key(230U, 0U);
    ASSERT_TRUE(base.AddCoin(original, CoinWithValue(50)));
    auto source = base.BeginBatch();
    ASSERT_TRUE(source.has_value());
    const nova::chain::UTXOView& view = *source;
    auto overlay = nova::chain::UTXOBatch::ReadOnlyOverlay(view);
    ASSERT_TRUE(overlay.has_value());
    const auto first = SpendingTransaction(original, 40);
    const auto created = UTXOKey{first.TxId(kLimits).value(), 0U};
    ASSERT_EQ(overlay->ApplyTransaction(first, 101U, false, kLimits).error, UTXOError::kNone);
    EXPECT_FALSE(overlay->HaveCoin(original));
    EXPECT_TRUE(overlay->HaveCoin(created));
    EXPECT_FALSE(overlay->Commit());
    EXPECT_TRUE(source->HaveCoin(original));
    EXPECT_FALSE(source->HaveCoin(created));
    {
        auto child = overlay->BeginBatch();
        ASSERT_TRUE(child.has_value());
        ASSERT_EQ(
            child->ApplyTransaction(SpendingTransaction(created, 30), 102U, false, kLimits).error,
            UTXOError::kNone);
        EXPECT_FALSE(child->Commit());
        EXPECT_TRUE(overlay->HaveCoin(created));
    }
    nova::chain::UTXOBatch moved{std::move(*overlay)};
    EXPECT_FALSE(overlay->IsCurrent());
    EXPECT_FALSE(moved.Commit());
    EXPECT_TRUE(moved.HaveCoin(created));
    EXPECT_TRUE(base.HaveCoin(original));
    EXPECT_EQ(base.size(), 1U);
}

TEST(UTXOReadOnly, ClosedAndStaleVersionedViewsRejectBeforeReadingPrivateDeltas)
{
    UTXOSet base;
    const auto original = Key(231U, 0U);
    ASSERT_TRUE(base.AddCoin(original, CoinWithValue(50)));
    auto source = base.BeginBatch();
    ASSERT_TRUE(source.has_value());
    auto overlay = nova::chain::UTXOBatch::ReadOnlyOverlay(*source);
    ASSERT_TRUE(overlay.has_value());
    const auto transaction = SpendingTransaction(original, 40);
    const auto created = UTXOKey{transaction.TxId(kLimits).value(), 0U};
    ASSERT_EQ(overlay->ApplyTransaction(transaction, 101U, false, kLimits).error, UTXOError::kNone);
    ASSERT_TRUE(base.AddCoin(Key(232U, 0U), CoinWithValue(1)));
    EXPECT_FALSE(source->IsCurrent());
    EXPECT_FALSE(overlay->IsCurrent());
    EXPECT_FALSE(source->GetCoin(original).has_value());
    EXPECT_FALSE(overlay->HaveCoin(created));
    EXPECT_FALSE(overlay->GetCoin(created).has_value());
    EXPECT_FALSE(nova::chain::UTXOBatch::ReadOnlyOverlay(*source).has_value());
    EXPECT_EQ(overlay->ApplyTransaction(transaction, 101U, false, kLimits).error,
              UTXOError::kBatchClosed);
    EXPECT_FALSE(overlay->Commit());
    auto fresh = base.BeginBatch();
    ASSERT_TRUE(fresh.has_value());
    fresh->Rollback();
    EXPECT_FALSE(fresh->HaveCoin(original));
    EXPECT_FALSE(fresh->GetCoin(original).has_value());
    EXPECT_FALSE(nova::chain::UTXOBatch::ReadOnlyOverlay(*fresh).has_value());
    EXPECT_TRUE(base.HaveCoin(original));
    EXPECT_EQ(base.size(), 2U);
}

} // namespace
