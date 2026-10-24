#include <gtest/gtest.h>

#include "consensus/monetary.hpp"
#include "consensus/network_params.hpp"
#include "consensus/pow.hpp"
#include "crypto/crypto.hpp"
#include "primitives/block.hpp"
#include "primitives/transaction.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using Record = std::map<std::string, std::string>;

std::filesystem::path VectorDirectory()
{
    return std::filesystem::path{NOVA_PROTOCOL_VECTOR_DIR};
}

std::optional<std::vector<std::uint8_t>> HexBytes(const std::string_view text)
{
    if (text.empty() || (text.size() % 2U) != 0U) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2U);
    const auto nibble = [](const char value) -> std::optional<std::uint8_t> {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint8_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<std::uint8_t>(value - 'a' + 10);
        }
        return std::nullopt;
    };
    for (std::size_t index = 0; index < text.size(); index += 2U) {
        const auto high = nibble(text[index]);
        const auto low = nibble(text[index + 1U]);
        if (!high.has_value() || !low.has_value()) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::uint8_t>((*high << 4U) | *low));
    }
    return bytes;
}

std::optional<Record> ReadRecord(const std::string_view filename)
{
    std::ifstream input{VectorDirectory() / filename, std::ios::binary};
    if (!input.is_open()) {
        return std::nullopt;
    }
    std::string line;
    if (!std::getline(input, line) || line != "NCPV1") {
        return std::nullopt;
    }

    Record record;
    std::string previous_name;
    bool terminated = false;
    while (std::getline(input, line)) {
        if (line == ".") {
            terminated = true;
            break;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0U ||
            line.find('=', separator + 1U) != std::string::npos) {
            return std::nullopt;
        }
        const auto name = line.substr(0U, separator);
        if (!std::islower(static_cast<unsigned char>(name.front())) || name <= previous_name ||
            !std::all_of(name.begin(), name.end(), [](const char value) {
                return std::islower(static_cast<unsigned char>(value)) ||
                       std::isdigit(static_cast<unsigned char>(value)) || value == '_';
            })) {
            return std::nullopt;
        }
        previous_name = name;
        record.emplace(name, line.substr(separator + 1U));
    }
    if (!terminated || record.empty() || std::getline(input, line)) {
        return std::nullopt;
    }
    return record;
}

const std::string& Field(const Record& record, const std::string_view name)
{
    const auto found = record.find(std::string{name});
    EXPECT_NE(found, record.end()) << "missing field " << name;
    static const std::string empty;
    return found == record.end() ? empty : found->second;
}

std::vector<nova::crypto::Hash256> ParseHashes(const std::string_view text)
{
    std::vector<nova::crypto::Hash256> hashes;
    std::size_t begin = 0U;
    while (begin < text.size()) {
        const auto end = text.find(',', begin);
        const auto item = text.substr(begin, end == std::string_view::npos ? text.size() - begin
                                                                            : end - begin);
        const auto hash = nova::crypto::Hash256::FromHex(item);
        EXPECT_TRUE(hash.has_value());
        if (!hash.has_value()) {
            return {};
        }
        hashes.push_back(*hash);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return hashes;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST(ProtocolCorpus, ManifestBindsEveryVectorFile)
{
    std::ifstream manifest{VectorDirectory() / "MANIFEST.sha256"};
    ASSERT_TRUE(manifest.is_open());
    std::map<std::string, std::string> expected;
    std::string hash;
    std::string marker_path;
    while (manifest >> hash >> marker_path) {
        ASSERT_EQ(hash.size(), 64U);
        ASSERT_TRUE(marker_path.starts_with('*'));
        expected.emplace(marker_path.substr(1U), hash);
    }
    ASSERT_FALSE(expected.empty());

    std::size_t actual_count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator{VectorDirectory()}) {
        if (entry.path().extension() != ".ncpv") {
            continue;
        }
        ++actual_count;
        const auto relative = entry.path().filename().string();
        const auto found = expected.find(relative);
        ASSERT_NE(found, expected.end()) << relative;
        const auto digest = nova::crypto::Hash256::Sha256(ReadFile(entry.path()));
        const auto expected_digest = nova::crypto::Hash256::FromHex(found->second);
        ASSERT_TRUE(digest.has_value());
        ASSERT_TRUE(expected_digest.has_value());
        EXPECT_EQ(*digest, *expected_digest);
    }
    EXPECT_EQ(actual_count, expected.size());
}

TEST(ProtocolConformance, HashAndSignaturePreimageVectors)
{
    for (const std::string_view filename : {std::string_view{"hashing.ncpv"},
                                            std::string_view{"sighash.ncpv"}}) {
        const auto record = ReadRecord(filename);
        ASSERT_TRUE(record.has_value()) << filename;
        const auto input = HexBytes(Field(*record, filename == std::string_view{"hashing.ncpv"} ? "input"
                                                                                           : "preimage"));
        const auto expected = nova::crypto::Hash256::FromHex(
            Field(*record, filename == std::string_view{"hashing.ncpv"} ? "output" : "digest"));
        ASSERT_TRUE(input.has_value());
        ASSERT_TRUE(expected.has_value());
        const auto actual = nova::crypto::Hash256::DoubleSha256(*input);
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(*actual, *expected);
    }
}

TEST(ProtocolConformance, TransactionSerializationVector)
{
    const auto record = ReadRecord("transactions.ncpv");
    ASSERT_TRUE(record.has_value());
    const auto serialized = HexBytes(Field(*record, "serialized"));
    const auto expected_id = nova::crypto::Hash256::FromHex(Field(*record, "txid"));
    ASSERT_TRUE(serialized.has_value());
    ASSERT_TRUE(expected_id.has_value());
    const auto transaction = nova::primitives::Transaction::Deserialize(
        *serialized, nova::consensus::TestnetNetworkParams().block_limits.transaction_limits);
    ASSERT_TRUE(transaction.has_value());
    const auto actual_id = transaction->TxId(nova::consensus::TestnetNetworkParams().block_limits.transaction_limits);
    ASSERT_TRUE(actual_id.has_value());
    EXPECT_EQ(*actual_id, *expected_id);
}

TEST(ProtocolConformance, MerkleAndTargetVectors)
{
    const auto merkle = ReadRecord("merkle.ncpv");
    ASSERT_TRUE(merkle.has_value());
    const auto expected_root = nova::crypto::Hash256::FromHex(Field(*merkle, "root"));
    ASSERT_TRUE(expected_root.has_value());
    const auto root = nova::primitives::ComputeMerkleRoot(ParseHashes(Field(*merkle, "txids")));
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(*root, *expected_root);

    const auto target = ReadRecord("target_pow.ncpv");
    ASSERT_TRUE(target.has_value());
    const auto compact = static_cast<std::uint32_t>(std::stoul(Field(*target, "bits"), nullptr, 16));
    const auto decoded = nova::consensus::TargetFromCompact(compact);
    ASSERT_EQ(decoded.error, nova::consensus::TargetError::kNone);
    ASSERT_TRUE(decoded.target.has_value());
    const auto expected_target = nova::consensus::Target256::FromHex(Field(*target, "target"));
    ASSERT_TRUE(expected_target.has_value());
    EXPECT_EQ(*decoded.target, *expected_target);
    EXPECT_EQ(nova::consensus::CompactFromTarget(*decoded.target), compact);
}

TEST(ProtocolConformance, SubsidyAndCommittedGenesisVectors)
{
    const auto subsidy = ReadRecord("subsidy.ncpv");
    ASSERT_TRUE(subsidy.has_value());
    const auto expected_subsidy = std::stoll(Field(*subsidy, "subsidy"));
    const auto height = static_cast<std::uint32_t>(std::stoul(Field(*subsidy, "height")));
    const auto actual_subsidy =
        nova::consensus::GetBlockSubsidy(height, nova::consensus::TestnetNetworkParams().monetary);
    ASSERT_TRUE(actual_subsidy.has_value());
    EXPECT_EQ(*actual_subsidy, expected_subsidy);

    const auto genesis = ReadRecord("genesis.ncpv");
    const auto block = ReadRecord("blocks.ncpv");
    ASSERT_TRUE(genesis.has_value());
    ASSERT_TRUE(block.has_value());
    const auto expected_hash = nova::crypto::Hash256::FromHex(Field(*genesis, "block_hash"));
    const auto expected_merkle = nova::crypto::Hash256::FromHex(Field(*genesis, "merkle_root"));
    const auto serialized = HexBytes(Field(*block, "serialized"));
    ASSERT_TRUE(expected_hash.has_value());
    ASSERT_TRUE(expected_merkle.has_value());
    ASSERT_TRUE(serialized.has_value());
    const auto parsed = nova::primitives::Block::Deserialize(
        *serialized, nova::consensus::TestnetNetworkParams().block_limits);
    ASSERT_TRUE(parsed.has_value());
    const auto actual_hash = nova::primitives::ComputeBlockHash(parsed->header);
    ASSERT_TRUE(actual_hash.has_value());
    EXPECT_EQ(*actual_hash, *expected_hash);
    EXPECT_EQ(parsed->header.merkle_root, *expected_merkle);
}

} // namespace
