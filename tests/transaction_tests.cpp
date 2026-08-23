#include <gtest/gtest.h>

#include "primitives/transaction.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace
{

using nova::primitives::Amount;
using nova::primitives::CheckTransactionStructure;
using nova::primitives::OutPoint;
using nova::primitives::Transaction;
using nova::primitives::TransactionLimits;
using nova::primitives::TransactionStructureError;
using nova::primitives::TxInput;
using nova::primitives::TxOutput;

constexpr TransactionLimits kLimits{21'000'000LL * 100'000'000LL, 1'000'000U, 16U, 16U, 256U};

nova::crypto::Hash256 HashWithLastByte(const std::uint8_t value)
{
    nova::crypto::Hash256::Bytes bytes{};
    bytes.back() = value;
    return nova::crypto::Hash256{bytes};
}

Transaction ValidTransaction()
{
    return Transaction{
        1, std::vector<TxInput>{TxInput{OutPoint{HashWithLastByte(1U), 2U}, {0x51U}, 0xFFFFFFFFU}},
        std::vector<TxOutput>{TxOutput{50U, {0x51U}}}, 0U};
}

TEST(TransactionSerialization, HasCanonicalByteLayoutAndStableTxId)
{
    const auto transaction = ValidTransaction();
    nova::primitives::BinaryWriter writer;
    ASSERT_TRUE(transaction.Serialize(writer, kLimits));

    std::vector<std::uint8_t> expected{0x01U, 0x00U, 0x00U, 0x00U, 0x01U};
    expected.insert(expected.end(), 31U, 0x00U);
    expected.insert(expected.end(), {0x01U, 0x02U, 0x00U, 0x00U, 0x00U, 0x01U, 0x51U, 0xFFU, 0xFFU,
                                     0xFFU, 0xFFU, 0x01U, 0x32U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                                     0x00U, 0x00U, 0x01U, 0x51U, 0x00U, 0x00U, 0x00U, 0x00U});
    const std::vector<std::uint8_t> serialized{writer.bytes().begin(), writer.bytes().end()};
    EXPECT_EQ(serialized, expected);
    const auto serialized_size = transaction.SerializedSize();
    const auto weight = transaction.Weight();
    ASSERT_TRUE(serialized_size.has_value());
    ASSERT_TRUE(weight.has_value());
    EXPECT_EQ(*serialized_size, static_cast<std::uint64_t>(expected.size()));
    EXPECT_EQ(*weight, static_cast<std::uint64_t>(expected.size()));

    const auto transaction_id = transaction.TxId(kLimits);
    const auto second_transaction_id = transaction.TxId(kLimits);
    const auto expected_transaction_id = nova::crypto::Hash256::FromHex(
        "dc5c9f99ffdd2edb05e0cd3c4b5576b661df48caea870796e753e67552846318");
    ASSERT_TRUE(transaction_id.has_value());
    ASSERT_TRUE(second_transaction_id.has_value());
    ASSERT_TRUE(expected_transaction_id.has_value());
    EXPECT_EQ(*transaction_id, *second_transaction_id);
    EXPECT_EQ(*transaction_id, *expected_transaction_id);

    const auto parsed = Transaction::Deserialize(writer.bytes(), kLimits);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->version, transaction.version);
    EXPECT_EQ(parsed->inputs.size(), transaction.inputs.size());
    EXPECT_EQ(parsed->outputs.size(), transaction.outputs.size());
    const auto parsed_transaction_id = parsed->TxId(kLimits);
    ASSERT_TRUE(parsed_transaction_id.has_value());
    EXPECT_EQ(*parsed_transaction_id, *transaction_id);
}

TEST(TransactionStructure, AcceptsAValidCoinbaseShapedTransaction)
{
    auto transaction = ValidTransaction();
    transaction.inputs.front().previous_output = OutPoint{nova::crypto::Hash256{}, 0xFFFFFFFFU};
    EXPECT_EQ(CheckTransactionStructure(transaction, kLimits), TransactionStructureError::kNone);
}

TEST(TransactionStructure, RejectsInvalidIndependentProperties)
{
    auto transaction = ValidTransaction();
    transaction.inputs.clear();
    EXPECT_EQ(CheckTransactionStructure(transaction, kLimits),
              TransactionStructureError::kZeroInputs);

    transaction = ValidTransaction();
    transaction.outputs.clear();
    EXPECT_EQ(CheckTransactionStructure(transaction, kLimits),
              TransactionStructureError::kZeroOutputs);

    transaction = ValidTransaction();
    transaction.inputs.push_back(transaction.inputs.front());
    EXPECT_EQ(CheckTransactionStructure(transaction, kLimits),
              TransactionStructureError::kDuplicateInput);

    transaction = ValidTransaction();
    transaction.inputs.front().previous_output = OutPoint{nova::crypto::Hash256{}, 1U};
    EXPECT_EQ(CheckTransactionStructure(transaction, kLimits),
              TransactionStructureError::kMalformedOutPoint);

    transaction = ValidTransaction();
    transaction.outputs.front().value = -1;
    EXPECT_EQ(CheckTransactionStructure(transaction, kLimits),
              TransactionStructureError::kNegativeOutputAmount);

    transaction = ValidTransaction();
    transaction.outputs.front().value = kLimits.max_money + 1;
    EXPECT_EQ(CheckTransactionStructure(transaction, kLimits),
              TransactionStructureError::kOutputAmountExceedsMaxMoney);

    transaction = ValidTransaction();
    transaction.outputs = {TxOutput{kLimits.max_money, {0x51U}}, TxOutput{1, {0x51U}}};
    EXPECT_EQ(CheckTransactionStructure(transaction, kLimits),
              TransactionStructureError::kTotalOutputExceedsMaxMoney);

    transaction = ValidTransaction();
    TransactionLimits overflow_limits = kLimits;
    overflow_limits.max_money = std::numeric_limits<Amount>::max();
    transaction.outputs = {TxOutput{std::numeric_limits<Amount>::max(), {0x51U}},
                           TxOutput{1, {0x51U}}};
    EXPECT_EQ(CheckTransactionStructure(transaction, overflow_limits),
              TransactionStructureError::kTotalOutputOverflow);
}

TEST(TransactionStructure, RejectsLimitsAndOversizeProperties)
{
    auto transaction = ValidTransaction();
    TransactionLimits invalid_limits = kLimits;
    invalid_limits.max_money = -1;
    EXPECT_EQ(CheckTransactionStructure(transaction, invalid_limits),
              TransactionStructureError::kInvalidLimits);

    TransactionLimits input_limit = kLimits;
    input_limit.max_inputs = 0U;
    EXPECT_EQ(CheckTransactionStructure(transaction, input_limit),
              TransactionStructureError::kTooManyInputs);

    TransactionLimits output_limit = kLimits;
    output_limit.max_outputs = 0U;
    EXPECT_EQ(CheckTransactionStructure(transaction, output_limit),
              TransactionStructureError::kTooManyOutputs);

    TransactionLimits script_limit = kLimits;
    script_limit.max_script_size = 0U;
    EXPECT_EQ(CheckTransactionStructure(transaction, script_limit),
              TransactionStructureError::kScriptSigTooLarge);

    transaction = ValidTransaction();
    transaction.outputs.front().script_pubkey = {0x51U, 0x51U};
    script_limit.max_script_size = 1U;
    EXPECT_EQ(CheckTransactionStructure(transaction, script_limit),
              TransactionStructureError::kScriptPubKeyTooLarge);

    transaction = ValidTransaction();
    TransactionLimits size_limit = kLimits;
    size_limit.max_serialized_size = 1U;
    EXPECT_EQ(CheckTransactionStructure(transaction, size_limit),
              TransactionStructureError::kOversizedTransaction);
}

TEST(TransactionParsing, RejectsMalformedAndNoncanonicalEncodings)
{
    const auto transaction = ValidTransaction();
    nova::primitives::BinaryWriter writer;
    ASSERT_TRUE(transaction.Serialize(writer, kLimits));
    std::vector<std::uint8_t> truncated{writer.bytes().begin(), writer.bytes().end()};
    truncated.pop_back();
    EXPECT_FALSE(Transaction::Deserialize(truncated, kLimits).has_value());

    std::vector<std::uint8_t> noncanonical{writer.bytes().begin(), writer.bytes().end()};
    noncanonical.at(4) = 0xFDU;
    noncanonical.insert(noncanonical.begin() + 5, {0x01U, 0x00U});
    EXPECT_FALSE(Transaction::Deserialize(noncanonical, kLimits).has_value());

    std::vector<std::uint8_t> trailing{writer.bytes().begin(), writer.bytes().end()};
    trailing.push_back(0x00U);
    EXPECT_FALSE(Transaction::Deserialize(trailing, kLimits).has_value());
}

} // namespace
