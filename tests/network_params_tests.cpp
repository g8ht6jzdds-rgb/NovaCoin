#include <gtest/gtest.h>

#include "consensus/network_params.hpp"
#include "primitives/serialization.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace
{

using nova::consensus::CheckNetworkParams;
using nova::consensus::GenerateGenesis;
using nova::consensus::GenesisError;
using nova::consensus::GenesisRequest;
using nova::consensus::MainnetNetworkParams;
using nova::consensus::NetworkId;
using nova::consensus::NetworkParamsError;
using nova::consensus::RegtestNetworkParams;
using nova::consensus::TestnetNetworkParams;

std::string Hex(const std::span<const std::uint8_t> bytes)
{
    constexpr std::string_view kDigits{"0123456789abcdef"};
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        result.push_back(kDigits[byte >> 4U]);
        result.push_back(kDigits[byte & 0x0FU]);
    }
    return result;
}

TEST(NetworkParams, CommitsDistinctRegtestAndTestnetFixtures)
{
    const auto& regtest = RegtestNetworkParams();
    const auto& testnet = TestnetNetworkParams();
    EXPECT_EQ(CheckNetworkParams(regtest), NetworkParamsError::kNone);
    EXPECT_EQ(CheckNetworkParams(testnet), NetworkParamsError::kNone);
    EXPECT_TRUE(regtest.enabled);
    EXPECT_FALSE(testnet.enabled);
    EXPECT_TRUE(testnet.deployment_final);
    EXPECT_NE(regtest.network_magic, testnet.network_magic);
    EXPECT_EQ(regtest.protocol_version, 1);
    EXPECT_EQ(regtest.minimum_peer_protocol_version, 1);
    EXPECT_EQ(testnet.protocol_version, 1);
    EXPECT_EQ(testnet.minimum_peer_protocol_version, 1);
    EXPECT_NE(regtest.default_p2p_port, testnet.default_p2p_port);
    EXPECT_NE(regtest.default_rpc_port, testnet.default_rpc_port);
    EXPECT_NE(regtest.address_prefixes.p2pkh, testnet.address_prefixes.p2pkh);
    EXPECT_NE(regtest.address_prefixes.private_key, testnet.address_prefixes.private_key);
    EXPECT_NE(regtest.pow.pow_limit, testnet.pow.pow_limit);
    EXPECT_NE(regtest.genesis_hash, testnet.genesis_hash);
}

TEST(NetworkParams, MatchesExactRegtestGenesisHashAndMerkleRoot)
{
    const auto& parameters = RegtestNetworkParams();
    const auto expected_hash = nova::crypto::Hash256::FromHex(
        "c974d11a5276ca7eb69b1ec0a8062fb47b49c02f5ac726532483d090ac53eb79");
    const auto expected_merkle = nova::crypto::Hash256::FromHex(
        "532198bb92e48c7059ddcb818874d5993a19fc6ee5c2809ff27e6bb07479a146");
    ASSERT_TRUE(expected_hash.has_value());
    ASSERT_TRUE(expected_merkle.has_value());
    EXPECT_EQ(parameters.genesis_hash, *expected_hash);
    EXPECT_EQ(parameters.genesis_merkle_root, *expected_merkle);
    EXPECT_EQ(parameters.genesis_block.header.merkle_root, *expected_merkle);
    EXPECT_EQ(parameters.genesis_block.header.nonce, 3U);
}

TEST(NetworkParams, MatchesExactTestnetGenesisHashAndMerkleRoot)
{
    const auto& parameters = TestnetNetworkParams();
    const auto expected_hash = nova::crypto::Hash256::FromHex(
        "25f944a00f3d559452b95653a20a039322ab3243a577d1cc3b8f48e4f30fd048");
    const auto expected_merkle = nova::crypto::Hash256::FromHex(
        "e0e0d43c6ef8f42f2e2d07eaf89b8566d76fbc1d698d826e5f4a26e6a3d7724c");
    ASSERT_TRUE(expected_hash.has_value());
    ASSERT_TRUE(expected_merkle.has_value());
    EXPECT_EQ(parameters.genesis_hash, *expected_hash);
    EXPECT_EQ(parameters.genesis_merkle_root, *expected_merkle);
    EXPECT_EQ(parameters.genesis_block.header.merkle_root, *expected_merkle);
    EXPECT_EQ(parameters.genesis_block.header.version, 1);
    EXPECT_EQ(parameters.genesis_block.header.time, 1'704'153'600U);
    EXPECT_EQ(parameters.genesis_block.header.bits, 0x2070'FFFFU);
    EXPECT_EQ(parameters.genesis_block.header.nonce, 0U);
    ASSERT_EQ(parameters.genesis_block.transactions.size(), 1U);
    const auto& coinbase = parameters.genesis_block.transactions.front();
    ASSERT_EQ(coinbase.inputs.size(), 1U);
    ASSERT_EQ(coinbase.outputs.size(), 1U);
    EXPECT_TRUE(coinbase.inputs.front().previous_output.IsNull());
    EXPECT_EQ(coinbase.inputs.front().script_sig,
              std::vector<std::uint8_t>({0x00U, 0x52U, 0x93U, 0x65U, 0x4EU, 0x6FU, 0x76U,
                                         0x61U, 0x43U, 0x6FU, 0x69U, 0x6EU, 0x20U, 0x54U,
                                         0x65U, 0x73U, 0x74U, 0x6EU, 0x65U, 0x74U, 0x20U,
                                         0x47U, 0x65U, 0x6EU, 0x65U, 0x73U, 0x69U, 0x73U}));
    EXPECT_EQ(coinbase.outputs.front().value, 50LL * nova::consensus::COIN);
    EXPECT_EQ(coinbase.outputs.front().script_pubkey, std::vector<std::uint8_t>({0x51U}));
}

TEST(NetworkParams, CommitsExactTestnetGenesisSerializationEvidence)
{
    const auto& parameters = TestnetNetworkParams();
    nova::primitives::BinaryWriter writer;
    ASSERT_TRUE(parameters.genesis_block.Serialize(writer, parameters.block_limits));
    EXPECT_EQ(Hex(writer.bytes()),
              "01000000"
              "0000000000000000000000000000000000000000000000000000000000000000"
              "e0e0d43c6ef8f42f2e2d07eaf89b8566d76fbc1d698d826e5f4a26e6a3d7724c"
              "00529365ffff70200000000001"
              "0100000001"
              "0000000000000000000000000000000000000000000000000000000000000000"
              "ffffffff1c005293654e6f7661436f696e20546573746e65742047656e65736973"
              "000000000100f2052a01000000015100000000");
    const auto expected_digest = nova::crypto::Hash256::FromHex(
        "04678f985ea3de5a84dffa8536218b65e599950c114034855e18434003014d63");
    const auto digest = nova::crypto::Hash256::DoubleSha256(writer.bytes());
    ASSERT_TRUE(expected_digest.has_value());
    ASSERT_TRUE(digest.has_value());
    EXPECT_EQ(*digest, *expected_digest);
}

TEST(NetworkParams, RejectsAlteredEnabledNetworkParameters)
{
    auto altered = RegtestNetworkParams();
    altered.default_rpc_port = 0U;
    EXPECT_EQ(CheckNetworkParams(altered), NetworkParamsError::kInvalidPorts);

    altered = RegtestNetworkParams();
    altered.genesis_block.header.nonce = 1U;
    EXPECT_EQ(CheckNetworkParams(altered), NetworkParamsError::kInvalidGenesisHash);

    altered = RegtestNetworkParams();
    altered.pow.pow_limit_compact = 0x2070'FFFFU;
    EXPECT_EQ(CheckNetworkParams(altered), NetworkParamsError::kInvalidPowParameters);

    altered = RegtestNetworkParams();
    altered.protocol_version = 0;
    EXPECT_EQ(CheckNetworkParams(altered), NetworkParamsError::kInvalidProtocolParameters);

    altered = RegtestNetworkParams();
    altered.minimum_peer_protocol_version = altered.protocol_version + 1;
    EXPECT_EQ(CheckNetworkParams(altered), NetworkParamsError::kInvalidProtocolParameters);
}

TEST(NetworkParams, RejectsTestnetActivationWithoutFinalApprovalFlag)
{
    auto altered = TestnetNetworkParams();
    altered.enabled = true;
    altered.deployment_final = false;
    EXPECT_EQ(CheckNetworkParams(altered), NetworkParamsError::kTestnetNotFinal);

    // The finalized review candidate remains unavailable until a separately
    // reviewed activation change sets enabled deliberately.
    EXPECT_FALSE(TestnetNetworkParams().enabled);
    EXPECT_TRUE(TestnetNetworkParams().deployment_final);
}

TEST(NetworkParams, FinalizedTestnetRemainsNonActivating)
{
    const auto& testnet = TestnetNetworkParams();
    const auto& mainnet = MainnetNetworkParams();
    EXPECT_TRUE(testnet.deployment_final);
    EXPECT_FALSE(testnet.enabled);
    EXPECT_EQ(CheckNetworkParams(testnet), NetworkParamsError::kNone);
    EXPECT_FALSE(mainnet.deployment_final);
    EXPECT_FALSE(mainnet.enabled);
}

TEST(NetworkParams, RejectsAnyTestnetGenesisMutation)
{
    auto altered = TestnetNetworkParams();
    altered.genesis_block.header.time += 1U;
    EXPECT_EQ(CheckNetworkParams(altered), NetworkParamsError::kInvalidGenesisHash);

    altered = TestnetNetworkParams();
    altered.genesis_block.header.bits = 0x2070'FFFEU;
    EXPECT_EQ(CheckNetworkParams(altered), NetworkParamsError::kInvalidGenesisTarget);

    altered = TestnetNetworkParams();
    altered.genesis_block.transactions.front().outputs.front().value += 1;
    EXPECT_EQ(CheckNetworkParams(altered), NetworkParamsError::kInvalidGenesisStructure);

    altered = TestnetNetworkParams();
    altered.genesis_block.transactions.front().inputs.front().script_sig.back() ^= 0x01U;
    EXPECT_EQ(CheckNetworkParams(altered), NetworkParamsError::kInvalidGenesisStructure);
}

TEST(NetworkParams, KeepsMainnetDisabledAndExplicitlyNonFinal)
{
    const auto& mainnet = MainnetNetworkParams();
    const auto& regtest = RegtestNetworkParams();
    const auto& testnet = TestnetNetworkParams();
    EXPECT_EQ(mainnet.id, NetworkId::kMainnet);
    EXPECT_FALSE(mainnet.enabled);
    EXPECT_FALSE(mainnet.deployment_final);
    EXPECT_EQ(CheckNetworkParams(mainnet), NetworkParamsError::kNone);
    EXPECT_NE(mainnet.network_magic, regtest.network_magic);
    EXPECT_NE(mainnet.network_magic, testnet.network_magic);
    EXPECT_NE(mainnet.default_p2p_port, regtest.default_p2p_port);
    EXPECT_NE(mainnet.default_rpc_port, testnet.default_rpc_port);
    EXPECT_NE(mainnet.address_prefixes.p2pkh, regtest.address_prefixes.p2pkh);
    EXPECT_NE(mainnet.address_prefixes.private_key, testnet.address_prefixes.private_key);
    EXPECT_NE(mainnet.pow.pow_limit, regtest.pow.pow_limit);
    EXPECT_NE(mainnet.pow.pow_limit, testnet.pow.pow_limit);
}

TEST(GenesisGenerator, ReproducesRegtestFixtureAndRejectsMalformedInput)
{
    const auto& parameters = RegtestNetworkParams();
    const auto generated =
        GenerateGenesis(GenesisRequest{1'704'067'200U, "NovaCoin Regtest Genesis", 0x207F'FFFFU,
                                       50LL * nova::consensus::COIN},
                        parameters.block_limits, 4U);
    ASSERT_EQ(generated.error, GenesisError::kNone);
    ASSERT_TRUE(generated.block_hash.has_value());
    ASSERT_TRUE(generated.merkle_root.has_value());
    EXPECT_EQ(*generated.block_hash, parameters.genesis_hash);
    EXPECT_EQ(*generated.merkle_root, parameters.genesis_merkle_root);
    EXPECT_EQ(generated.attempts, 4U);

    const auto invalid_message =
        GenerateGenesis(GenesisRequest{1U, "\x01", 0x207F'FFFFU, 1}, parameters.block_limits, 1U);
    EXPECT_EQ(invalid_message.error, GenesisError::kInvalidMessage);
    const auto invalid_target =
        GenerateGenesis(GenesisRequest{1U, "valid", 0x1D80'FFFFU, 1}, parameters.block_limits, 1U);
    EXPECT_EQ(invalid_target.error, GenesisError::kInvalidTarget);
    const auto invalid_reward =
        GenerateGenesis(GenesisRequest{1U, "valid", 0x207F'FFFFU, -1}, parameters.block_limits, 1U);
    EXPECT_EQ(invalid_reward.error, GenesisError::kInvalidReward);
}

TEST(GenesisGenerator, ReproducesTestnetCandidateFixture)
{
    const auto& parameters = TestnetNetworkParams();
    const auto generated =
        GenerateGenesis(GenesisRequest{1'704'153'600U, "NovaCoin Testnet Genesis", 0x2070'FFFFU,
                                       50LL * nova::consensus::COIN},
                        parameters.block_limits, 1U);
    ASSERT_EQ(generated.error, GenesisError::kNone);
    ASSERT_TRUE(generated.block.has_value());
    ASSERT_TRUE(generated.block_hash.has_value());
    ASSERT_TRUE(generated.merkle_root.has_value());
    EXPECT_EQ(generated.block->header.nonce, 0U);
    EXPECT_EQ(generated.attempts, 1U);
    EXPECT_EQ(*generated.block_hash, parameters.genesis_hash);
    EXPECT_EQ(*generated.merkle_root, parameters.genesis_merkle_root);
}

} // namespace
