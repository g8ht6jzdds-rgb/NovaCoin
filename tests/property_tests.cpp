#include <gtest/gtest.h>

#include "chain/chainstate.hpp"
#include "chain/utxo.hpp"
#include "consensus/pow.hpp"
#include "primitives/serialization.hpp"

#include <cstdint>
#include <vector>

namespace
{

constexpr nova::primitives::TransactionLimits kLimits{21'000'000LL * 100'000'000LL, 100'000U, 8U,
                                                      8U, 128U};

std::uint64_t Next(std::uint64_t& state)
{
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

nova::crypto::Hash256 Hash(const std::uint8_t marker)
{
    nova::crypto::Hash256::Bytes bytes{};
    bytes.back() = marker;
    return nova::crypto::Hash256{bytes};
}

TEST(ConsensusProperties, SerializationRoundTripsDeterministically)
{
    std::uint64_t seed = 0x4E4F5641434F494EULL;
    for (std::size_t iteration = 0U; iteration < 1'000U; ++iteration) {
        const auto input = Next(seed);
        nova::primitives::BinaryWriter writer;
        ASSERT_TRUE(writer.WriteU64(input));
        nova::primitives::BinaryReader reader{writer.bytes()};
        const auto output = reader.ReadU64();
        ASSERT_TRUE(output.has_value());
        EXPECT_EQ(*output, input);
        EXPECT_TRUE(reader.RequireEnd());
    }
}

TEST(ConsensusProperties, MoneyConservationAndUtxoUndoAreSymmetric)
{
    std::uint64_t seed = 0xC0FFEEU;
    for (std::uint8_t marker = 1U; marker < 64U; ++marker) {
        nova::chain::UTXOSet utxos;
        const nova::chain::UTXOKey key{Hash(marker), 0U};
        const auto input_units = 100U + Next(seed) % 10'000U;
        const auto input_value = static_cast<nova::primitives::Amount>(input_units);
        ASSERT_TRUE(utxos.AddCoin(key, {{input_value, {0x51U}}, 1U, false}));
        const auto fee = static_cast<nova::primitives::Amount>(Next(seed) % input_units);
        const nova::primitives::Transaction transaction{
            1,
            {{{key.transaction_id, key.output_index}, {0x51U}, 0xFFFF'FFFFU}},
            {{input_value - fee, {0x51U}}},
            0U};
        ASSERT_EQ(nova::primitives::CheckTransactionStructure(transaction, kLimits),
                  nova::primitives::TransactionStructureError::kNone);
        const auto applied = utxos.ApplyTransaction(transaction, 2U, false, kLimits);
        ASSERT_TRUE(applied.undo.has_value());
        ASSERT_EQ(applied.error, nova::chain::UTXOError::kNone);
        EXPECT_EQ(input_value, transaction.outputs.front().value + fee);
        ASSERT_EQ(utxos.UndoTransaction(*applied.undo), nova::chain::UTXOError::kNone);
        EXPECT_TRUE(utxos.HaveCoin(key));
        EXPECT_EQ(utxos.size(), 1U);
    }
}

TEST(ConsensusProperties, BlockConnectDisconnectRestoresUtxoState)
{
    nova::chain::UTXOSet utxos;
    const nova::chain::UTXOKey input{Hash(0xA1U), 0U};
    ASSERT_TRUE(utxos.AddCoin(input, {{100, {0x51U}}, 1U, false}));
    const nova::primitives::Transaction transaction{
        1, {{{input.transaction_id, 0U}, {0x51U}, 0xFFFF'FFFFU}}, {{99, {0x51U}}}, 0U};
    const std::vector<nova::primitives::Transaction> block{transaction};
    const auto applied = utxos.ApplyBlock(block, 2U, kLimits);
    ASSERT_EQ(applied.error, nova::chain::UTXOError::kNone);
    ASSERT_TRUE(applied.undo.has_value());
    ASSERT_EQ(utxos.UndoBlock(*applied.undo), nova::chain::UTXOError::kNone);
    EXPECT_TRUE(utxos.HaveCoin(input));
    EXPECT_EQ(utxos.size(), 1U);
}

TEST(ConsensusProperties, ChainworkIsStrictlyMonotonicForPositiveWork)
{
    const auto target = nova::consensus::TargetFromCompact(0x207F'FFFFU);
    ASSERT_TRUE(target.target.has_value());
    const auto work = nova::consensus::CalculateWork(*target.target);
    ASSERT_TRUE(work.has_value());
    auto chainwork = nova::chain::ChainWork::FromPerBlockWork(*work);
    const auto before = chainwork;
    ASSERT_TRUE(chainwork.Add(*work));
    EXPECT_TRUE(before < chainwork);
}

} // namespace
