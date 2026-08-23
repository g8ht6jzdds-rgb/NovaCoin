#include <gtest/gtest.h>

#include "primitives/block.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace
{

using nova::primitives::Block;
using nova::primitives::BlockLimits;
using nova::primitives::BlockStructureError;
using nova::primitives::ComputeMerkleRoot;
using nova::primitives::OutPoint;
using nova::primitives::Transaction;
using nova::primitives::TransactionLimits;
using nova::primitives::TxInput;
using nova::primitives::TxOutput;

constexpr TransactionLimits kTransactionLimits{21'000'000LL * 100'000'000LL, 1'000'000U, 16U, 16U,
                                               256U};
constexpr BlockLimits kBlockLimits{kTransactionLimits, 1'000'000U, 16U, 64U};

nova::crypto::Hash256 HashFromHex(const char* hex)
{
    const auto hash = nova::crypto::Hash256::FromHex(hex);
    EXPECT_TRUE(hash.has_value());
    return hash.value_or(nova::crypto::Hash256{});
}

nova::crypto::Hash256 HashWithLastByte(const std::uint8_t value)
{
    nova::crypto::Hash256::Bytes bytes{};
    bytes.back() = value;
    return nova::crypto::Hash256{bytes};
}

Transaction CoinbaseTransaction()
{
    return Transaction{1,
                       std::vector<TxInput>{TxInput{
                           OutPoint{nova::crypto::Hash256{}, 0xFFFFFFFFU}, {0x01U, 0x00U}, 0U}},
                       std::vector<TxOutput>{TxOutput{50U, {0x51U}}}, 0U};
}

Transaction RegularTransaction(const std::uint8_t marker)
{
    return Transaction{
        1,
        std::vector<TxInput>{TxInput{OutPoint{HashWithLastByte(marker), 0U}, {0x51U}, 0xFFFFFFFFU}},
        std::vector<TxOutput>{TxOutput{40U, {0x51U}}}, 0U};
}

Block ValidBlock(std::vector<Transaction> transactions)
{
    const auto merkle_root = ComputeMerkleRoot(transactions, kTransactionLimits);
    EXPECT_TRUE(merkle_root.has_value());
    return Block{{1, nova::crypto::Hash256{}, *merkle_root, 1'700'000'000U, 0U, 0U},
                 std::move(transactions)};
}

TEST(BlockMerkle, DeterministicVectorsForOneTwoThreeAndOddCounts)
{
    const std::array<nova::crypto::Hash256, 4> transaction_ids{
        HashWithLastByte(1U), HashWithLastByte(2U), HashWithLastByte(3U), HashWithLastByte(4U)};
    const auto one =
        ComputeMerkleRoot(std::span<const nova::crypto::Hash256>{transaction_ids}.first(1));
    const auto two =
        ComputeMerkleRoot(std::span<const nova::crypto::Hash256>{transaction_ids}.first(2));
    const auto three =
        ComputeMerkleRoot(std::span<const nova::crypto::Hash256>{transaction_ids}.first(3));
    const auto four = ComputeMerkleRoot(std::span<const nova::crypto::Hash256>{transaction_ids});
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(two.has_value());
    ASSERT_TRUE(three.has_value());
    ASSERT_TRUE(four.has_value());
    EXPECT_EQ(*one,
              HashFromHex("0000000000000000000000000000000000000000000000000000000000000001"));
    EXPECT_EQ(*two,
              HashFromHex("9b1eb2a9e9a81cbbad8edffa901e93fd94f4405e63b61647cf9d3e602b1fb7b0"));
    EXPECT_EQ(*three,
              HashFromHex("0de7d95afed92f912d7004de0371e290261027e3ec5ef7846a535881454e5c36"));
    EXPECT_EQ(*four,
              HashFromHex("2c76ecc1f6a379b82aadc24b14cded50e6b59693b02cee76342c15cf0e31b700"));
}

TEST(BlockMerkle, ChangesForChangedTransactionAndOrdering)
{
    const std::array<nova::crypto::Hash256, 3> original{HashWithLastByte(1U), HashWithLastByte(2U),
                                                        HashWithLastByte(3U)};
    const std::array<nova::crypto::Hash256, 3> changed{HashWithLastByte(1U), HashWithLastByte(2U),
                                                       HashWithLastByte(4U)};
    const std::array<nova::crypto::Hash256, 3> reordered{HashWithLastByte(2U), HashWithLastByte(1U),
                                                         HashWithLastByte(3U)};
    const auto original_root = ComputeMerkleRoot(original);
    const auto changed_root = ComputeMerkleRoot(changed);
    const auto reordered_root = ComputeMerkleRoot(reordered);
    ASSERT_TRUE(original_root.has_value());
    ASSERT_TRUE(changed_root.has_value());
    ASSERT_TRUE(reordered_root.has_value());
    EXPECT_EQ(*changed_root,
              HashFromHex("4730f443086fbb93a3c2e883f81d915bee8e7dac7163cc02683aef6a81aaf810"));
    EXPECT_EQ(*reordered_root,
              HashFromHex("36c0581bd0a138a96ae9db8933d7da4fb4ea6de64fd72c8a7a7d6866554ee4bf"));
    EXPECT_NE(*original_root, *changed_root);
    EXPECT_NE(*original_root, *reordered_root);
}

TEST(BlockStructure, SerializesParsesAndHashesCanonicalBlocks)
{
    auto block = ValidBlock({CoinbaseTransaction(), RegularTransaction(1U)});
    EXPECT_EQ(nova::primitives::CheckBlockStructure(block, kBlockLimits),
              BlockStructureError::kNone);
    nova::primitives::BinaryWriter writer;
    ASSERT_TRUE(block.Serialize(writer, kBlockLimits));
    const auto serialized_size = block.SerializedSize();
    ASSERT_TRUE(serialized_size.has_value());
    EXPECT_EQ(writer.bytes().size(), static_cast<std::size_t>(*serialized_size));
    const auto parsed = Block::Deserialize(writer.bytes(), kBlockLimits);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->transactions.size(), 2U);
    EXPECT_EQ(parsed->header.merkle_root, block.header.merkle_root);
    const auto first_hash = nova::primitives::ComputeBlockHash(block.header);
    const auto second_hash = nova::primitives::ComputeBlockHash(parsed->header);
    ASSERT_TRUE(first_hash.has_value());
    ASSERT_TRUE(second_hash.has_value());
    EXPECT_EQ(*first_hash, *second_hash);
}

TEST(BlockStructure, RejectsInvalidCoinbaseMerkleDuplicateAndSizeRules)
{
    auto no_transactions = ValidBlock({CoinbaseTransaction()});
    no_transactions.transactions.clear();
    EXPECT_EQ(nova::primitives::CheckBlockStructure(no_transactions, kBlockLimits),
              BlockStructureError::kNoTransactions);

    auto first_not_coinbase = ValidBlock({CoinbaseTransaction(), RegularTransaction(2U)});
    std::swap(first_not_coinbase.transactions.at(0), first_not_coinbase.transactions.at(1));
    EXPECT_EQ(nova::primitives::CheckBlockStructure(first_not_coinbase, kBlockLimits),
              BlockStructureError::kFirstTransactionNotCoinbase);

    auto second_coinbase = CoinbaseTransaction();
    second_coinbase.outputs.front().value = 49;
    auto multiple_coinbase = ValidBlock({CoinbaseTransaction(), second_coinbase});
    EXPECT_EQ(nova::primitives::CheckBlockStructure(multiple_coinbase, kBlockLimits),
              BlockStructureError::kMultipleCoinbaseTransactions);

    auto invalid_coinbase_script = ValidBlock({CoinbaseTransaction()});
    invalid_coinbase_script.transactions.front().inputs.front().script_sig = {0x01U};
    EXPECT_EQ(nova::primitives::CheckBlockStructure(invalid_coinbase_script, kBlockLimits),
              BlockStructureError::kInvalidCoinbaseScriptSize);

    auto bad_merkle = ValidBlock({CoinbaseTransaction(), RegularTransaction(3U)});
    bad_merkle.header.merkle_root = nova::crypto::Hash256{};
    EXPECT_EQ(nova::primitives::CheckBlockStructure(bad_merkle, kBlockLimits),
              BlockStructureError::kBadMerkleRoot);

    const auto duplicate_regular = RegularTransaction(4U);
    auto duplicate_ids = ValidBlock({CoinbaseTransaction(), duplicate_regular, duplicate_regular});
    EXPECT_EQ(nova::primitives::CheckBlockStructure(duplicate_ids, kBlockLimits),
              BlockStructureError::kDuplicateTransactionId);

    auto oversized = ValidBlock({CoinbaseTransaction()});
    auto small_limits = kBlockLimits;
    small_limits.max_serialized_size = 1U;
    EXPECT_EQ(nova::primitives::CheckBlockStructure(oversized, small_limits),
              BlockStructureError::kOversizedBlock);
}

} // namespace
