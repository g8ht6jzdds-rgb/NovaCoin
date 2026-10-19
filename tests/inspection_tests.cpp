#include <gtest/gtest.h>

#include "inspection/chain_report.hpp"
#include "storage/network_identity.hpp"

#include <limits>
#include <sstream>

namespace
{
using namespace nova;

inspection::InspectionContext
Context(const consensus::NetworkId network = consensus::NetworkId::kRegtest)
{
    auto state = chain::NetworkChainInitializer::Create(
        network, std::make_shared<chain::FixedValidationTimeSource>(1'704'160'000U));
    EXPECT_TRUE(state.has_value());
    const auto tip = state->chain->GetBlockIndex(state->chain->active_tip());
    EXPECT_TRUE(tip.has_value());
    return {network,
            1'704'160'000U,
            tip->hash,
            0U,
            tip->chain_work,
            std::string(40U, 'a'),
            std::string(64U, 'b'),
            std::string(64U, 'c'),
            std::string(64U, 'd'),
            std::string(64U, 'e'),
            std::string(64U, 'f')};
}

std::string Journal(const std::vector<primitives::Block>& blocks)
{
    primitives::BinaryWriter output;
    const auto& limits = consensus::RegtestNetworkParams().block_limits;
    for (const auto& block : blocks) {
        primitives::BinaryWriter payload;
        EXPECT_TRUE(block.Serialize(payload, limits));
        EXPECT_TRUE(output.WriteU8(1U));
        EXPECT_TRUE(output.WriteU32(static_cast<std::uint32_t>(payload.bytes().size())));
        // Journal payload is raw bytes, not a compact-size-prefixed byte string.
        for (const auto byte : payload.bytes())
            EXPECT_TRUE(output.WriteU8(byte));
        EXPECT_TRUE(output.WriteU8(2U));
        const auto hash = primitives::ComputeBlockHash(block.header);
        EXPECT_TRUE(hash.has_value());
        EXPECT_TRUE(output.WriteHash256(*hash));
    }
    const auto bytes = output.bytes();
    return {bytes.begin(), bytes.end()};
}

std::optional<primitives::Block>
Block(const crypto::Hash256& parent, const std::uint32_t height, const std::uint32_t marker,
      const primitives::Amount reward, std::vector<std::uint8_t> locking = {0x51U},
      const std::optional<primitives::Transaction>& spend = std::nullopt)
{
    const auto& network = consensus::RegtestNetworkParams();
    primitives::Transaction coinbase{
        1,
        {{{{}, 0xffff'ffffU},
          {1U, static_cast<std::uint8_t>(height), static_cast<std::uint8_t>(marker)},
          0U}},
        {{reward, std::move(locking)}},
        0U};
    primitives::Block block{{1,
                             parent,
                             {},
                             network.genesis_block.header.time + marker,
                             network.genesis_block.header.bits,
                             0U},
                            {std::move(coinbase)}};
    if (spend.has_value())
        block.transactions.push_back(*spend);
    const auto root =
        primitives::ComputeMerkleRoot(block.transactions, network.block_limits.transaction_limits);
    if (!root.has_value())
        return std::nullopt;
    block.header.merkle_root = *root;
    const auto mined = consensus::MineRegtestBlock(block.header, 4096U);
    if (!mined.has_value())
        return std::nullopt;
    block.header = mined->header;
    return block;
}

inspection::InspectionContext Expected(const std::vector<primitives::Block>& blocks)
{
    auto result = Context();
    auto state = chain::NetworkChainInitializer::Create(
        result.network, std::make_shared<chain::FixedValidationTimeSource>(result.as_of));
    EXPECT_TRUE(state.has_value());
    for (const auto& block : blocks)
        EXPECT_EQ(state->chain->AcceptBlock(block).error, chain::ChainError::kNone);
    const auto tip = state->chain->GetBlockIndex(state->chain->active_tip());
    EXPECT_TRUE(tip.has_value());
    result.expected_height = tip->height;
    result.expected_tip = tip->hash;
    result.expected_work = tip->chain_work;
    return result;
}

TEST(OfflineInspection, EmptyRegtestAndFrozenTestnetAreDeterministic)
{
    for (const auto network : {consensus::NetworkId::kRegtest, consensus::NetworkId::kTestnet}) {
        std::istringstream a, b;
        const auto context = Context(network);
        const auto first = inspection::InspectJournal(a, context);
        const auto second = inspection::InspectJournal(b, context);
        EXPECT_TRUE(first.error.empty()) << first.error;
        EXPECT_EQ(first.report, second.report);
        EXPECT_NE(first.report.find(network == consensus::NetworkId::kTestnet
                                        ? "\"utxo_value\":\"1000000000000000\""
                                        : "\"utxo_value\":\"0\""),
                  std::string::npos);
        EXPECT_NE(first.report.find("OFFLINE_SNAPSHOT_VALID"), std::string::npos);
    }
}

TEST(OfflineInspection, BoundedSharedDecoderPreservesRecoveryDiagnostics)
{
    const auto& network = consensus::RegtestNetworkParams();
    const auto block = Block(network.genesis_hash, 1U, 1U, consensus::INITIAL_SUBSIDY);
    ASSERT_TRUE(block.has_value());
    const auto bytes = Journal({*block});
    std::istringstream exact{bytes}, small{bytes}, records{bytes}, pending{bytes}, blocks{bytes};
    EXPECT_EQ(
        storage::ReadBlockJournal(exact, network.block_limits, {bytes.size(), 2U, 1U, 1U}).error,
        storage::BlockJournalError::kNone);
    EXPECT_EQ(
        storage::ReadBlockJournal(small, network.block_limits, {bytes.size() - 1U, 2U, 1U, 1U})
            .error,
        storage::BlockJournalError::kResourceLimit);
    EXPECT_EQ(
        storage::ReadBlockJournal(records, network.block_limits, {bytes.size(), 1U, 1U, 1U}).error,
        storage::BlockJournalError::kResourceLimit);
    EXPECT_EQ(
        storage::ReadBlockJournal(pending, network.block_limits, {bytes.size(), 2U, 1U, 0U}).error,
        storage::BlockJournalError::kResourceLimit);
    EXPECT_EQ(
        storage::ReadBlockJournal(blocks, network.block_limits, {bytes.size(), 2U, 0U, 1U}).error,
        storage::BlockJournalError::kResourceLimit);
    auto context = Expected({*block});
    for (std::size_t size = 1U; size < bytes.size(); ++size) {
        std::istringstream cut{bytes.substr(0U, size)};
        const auto result = inspection::InspectJournal(cut, context);
        EXPECT_FALSE(result.error.empty()) << size;
        EXPECT_TRUE(result.report.empty());
    }
    std::istringstream io;
    io.setstate(std::ios::badbit);
    EXPECT_NE(storage::ReadBlockJournal(io, network.block_limits).error,
              storage::BlockJournalError::kNone);
}

TEST(OfflineInspection, WinningBranchOnlyAndResourceOrExpectedMismatchNeverCompletes)
{
    const auto& network = consensus::RegtestNetworkParams();
    const auto a = Block(network.genesis_hash, 1U, 1U, consensus::INITIAL_SUBSIDY);
    const auto b = Block(network.genesis_hash, 1U, 2U, 0);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    const auto bh = primitives::ComputeBlockHash(b->header);
    ASSERT_TRUE(bh.has_value());
    const auto c = Block(*bh, 2U, 3U, consensus::INITIAL_SUBSIDY);
    ASSERT_TRUE(c.has_value());
    const auto bytes = Journal({*a, *b, *c});
    auto context = Expected({*a, *b, *c});
    std::istringstream stream{bytes};
    const auto result = inspection::InspectJournal(stream, context);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_NE(result.report.find("\"utxo_value\":\"5000000000\""), std::string::npos);
    EXPECT_NE(result.report.find("\"utxo_count\":\"2\""), std::string::npos);
    for (const auto limits : {inspection::InspectionLimits{{}, 1U, 100'000U},
                              inspection::InspectionLimits{{}, 1'000'000U, 10U}}) {
        std::istringstream input{bytes};
        EXPECT_FALSE(inspection::InspectJournal(input, context, limits).error.empty());
    }
    context.expected_height++;
    std::istringstream wrong{bytes};
    EXPECT_EQ(inspection::InspectJournal(wrong, context).error, "snapshot_expectation");
    std::istringstream duplicate{Journal({*a, *a})};
    EXPECT_FALSE(inspection::InspectJournal(duplicate, context).error.empty());
}

TEST(OfflineInspection, SyntheticSignedFeesAndUnderclaimAreNotDoubleCounted)
{
    // Known synthetic key 1, never the frozen TESTNET creator key.
    crypto::PrivateKey::Bytes bytes{};
    bytes.back() = 1U;
    const auto key = crypto::PrivateKey::FromBytes(bytes);
    ASSERT_TRUE(key.has_value());
    const auto pub = key->DerivePublicKey();
    ASSERT_TRUE(pub.has_value());
    const auto hash = crypto::Hash160Digest(pub->SerializeCompressed());
    ASSERT_TRUE(hash.has_value());
    std::vector<std::uint8_t> script{0x76U, 0xa9U, 0x14U};
    script.insert(script.end(), hash->begin(), hash->end());
    script.insert(script.end(), {0x88U, 0xacU});
    const auto& network = consensus::RegtestNetworkParams();
    const auto a = Block(network.genesis_hash, 1U, 1U, consensus::INITIAL_SUBSIDY, script);
    ASSERT_TRUE(a.has_value());
    const auto id = a->transactions.front().TxId(network.block_limits.transaction_limits);
    ASSERT_TRUE(id.has_value());
    primitives::Transaction spend{
        1, {{{*id, 0U}, {}, 0xffff'ffffU}}, {{49 * consensus::COIN, script}, {0, {0x6aU}}}, 0U};
    const auto digest =
        chain::ComputeSignatureHash(spend, 0U, script, network.block_limits.transaction_limits, 1U);
    ASSERT_TRUE(digest.has_value());
    const auto signature = key->Sign(*digest);
    ASSERT_TRUE(signature.has_value());
    const auto der = signature->SerializeDer();
    auto& unlock = spend.inputs.front().script_sig;
    unlock.push_back(static_cast<std::uint8_t>(der.size() + 1U));
    unlock.insert(unlock.end(), der.begin(), der.end());
    unlock.insert(unlock.end(), {1U, 33U});
    const auto serialized = pub->SerializeCompressed();
    unlock.insert(unlock.end(), serialized.begin(), serialized.end());
    const auto ah = primitives::ComputeBlockHash(a->header);
    ASSERT_TRUE(ah.has_value());
    const auto b = Block(*ah, 2U, 2U, 0, {0x51U}, spend);
    ASSERT_TRUE(b.has_value());
    std::istringstream input{Journal({*a, *b})};
    const auto result = inspection::InspectJournal(input, Expected({*a, *b}));
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_NE(result.report.find("\"utxo_value\":\"4900000000\""), std::string::npos);
    EXPECT_NE(result.report.find("\"fees\":\"100000000\""), std::string::npos);
    EXPECT_NE(result.report.find("\"utxo_count\":\"3\""), std::string::npos);
    EXPECT_NE(result.report.find("\"combined_underclaim\":\"5100000000\""), std::string::npos);
    auto invalid = *b;
    invalid.transactions.back().outputs.front().value++;
    const auto root = primitives::ComputeMerkleRoot(invalid.transactions,
                                                    network.block_limits.transaction_limits);
    ASSERT_TRUE(root.has_value());
    invalid.header.merkle_root = *root;
    const auto mined = consensus::MineRegtestBlock(invalid.header, 4096U);
    ASSERT_TRUE(mined.has_value());
    invalid.header = mined->header;
    std::istringstream rejected{Journal({*a, invalid})};
    EXPECT_FALSE(inspection::InspectJournal(rejected, Expected({*a, *b})).error.empty());
}

TEST(OfflineInspection, CheckedReportingArithmeticAndInvalidContext)
{
    auto value = std::numeric_limits<std::int64_t>::max();
    EXPECT_FALSE(inspection::CheckedAdd(value, 1));
    value = std::numeric_limits<std::int64_t>::min();
    EXPECT_FALSE(inspection::CheckedAdd(value, -1));
    auto context = Context();
    context.source_revision = "UNRECORDED";
    std::istringstream empty;
    EXPECT_EQ(inspection::InspectJournal(empty, context).error, "context");
    std::array<std::uint8_t, 45U> marker{};
    EXPECT_NE(storage::ValidateNetworkIdentity(marker, consensus::RegtestNetworkParams()),
              storage::NetworkIdentityError::kNone);
}
} // namespace
