#include <gtest/gtest.h>

#include "chain/network_chainstate.hpp"
#include "chain/validation.hpp"
#include "consensus/monetary.hpp"
#include "consensus/network_params.hpp"
#include "consensus/pow.hpp"
#include "crypto/crypto.hpp"
#include "primitives/block.hpp"
#include "primitives/transaction.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
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

std::string HexText(const std::span<const std::uint8_t> bytes)
{
    static constexpr std::string_view kHex{"0123456789abcdef"};
    std::string text;
    text.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        text.push_back(kHex.at(byte >> 4U));
        text.push_back(kHex.at(byte & 0x0FU));
    }
    return text;
}

std::optional<Record> ParseRecord(std::istream& input)
{
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

std::optional<Record> ReadRecord(const std::string_view filename)
{
    std::ifstream input{VectorDirectory() / filename, std::ios::binary};
    if (!input.is_open()) {
        return std::nullopt;
    }
    return ParseRecord(input);
}

const std::string& Field(const Record& record, const std::string_view name)
{
    const auto found = record.find(std::string{name});
    EXPECT_NE(found, record.end()) << "missing field " << name;
    static const std::string empty;
    return found == record.end() ? empty : found->second;
}

template <typename Unsigned>
std::optional<Unsigned> ParseCanonicalUnsigned(const std::string_view text)
{
    if (text.empty() || (text.size() > 1U && text.front() == '0') ||
        !std::all_of(text.begin(), text.end(),
                     [](const char value) { return value >= '0' && value <= '9'; })) {
        return std::nullopt;
    }
    Unsigned value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<nova::primitives::Amount> ParseCanonicalAmount(const std::string_view text)
{
    if (text.empty() || (text.size() > 1U && text.front() == '0') ||
        !std::all_of(text.begin(), text.end(),
                     [](const char value) { return value >= '0' && value <= '9'; })) {
        return std::nullopt;
    }
    nova::primitives::Amount value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<nova::chain::BlockValidationError>
SignatureAuthorizationResult(const std::string_view value)
{
    using nova::chain::BlockValidationError;
    if (value == "accept") {
        return BlockValidationError::kNone;
    }
    if (value == "malformed_unlocking_script") {
        return BlockValidationError::kMalformedUnlockingScript;
    }
    if (value == "invalid_public_key") {
        return BlockValidationError::kInvalidPublicKey;
    }
    if (value == "invalid_signature") {
        return BlockValidationError::kInvalidSignature;
    }
    return std::nullopt;
}

struct SignatureAuthorizationRecord final {
    nova::primitives::Transaction transaction;
    std::size_t input_index{};
    nova::chain::Coin referenced_coin;
    nova::chain::BlockValidationError expected_result{};
};

std::optional<SignatureAuthorizationRecord> ParseSignatureAuthorizationRecord(const Record& record)
{
    static constexpr std::array<std::string_view, 13U> kFields{"expected_result",
                                                               "id",
                                                               "input_index",
                                                               "kind",
                                                               "network",
                                                               "referenced_output_script",
                                                               "referenced_output_value",
                                                               "sighash_selector",
                                                               "source_refs",
                                                               "spec_section",
                                                               "status",
                                                               "transaction",
                                                               "unlocking_script"};

    if (record.size() != kFields.size()) {
        return std::nullopt;
    }
    for (const auto field : kFields) {
        const auto found = record.find(std::string{field});
        if (found == record.end() || found->second.empty()) {
            return std::nullopt;
        }
    }
    if (Field(record, "kind") != "signature_authorization" ||
        Field(record, "network") != "REGTEST" || Field(record, "sighash_selector") != "1") {
        return std::nullopt;
    }
    const auto expected = SignatureAuthorizationResult(Field(record, "expected_result"));
    const auto input_index = ParseCanonicalUnsigned<std::uint64_t>(Field(record, "input_index"));
    const auto value = ParseCanonicalAmount(Field(record, "referenced_output_value"));
    const auto script = HexBytes(Field(record, "referenced_output_script"));
    const auto unlocking_script = HexBytes(Field(record, "unlocking_script"));
    const auto serialized = HexBytes(Field(record, "transaction"));
    if (!expected.has_value() || !input_index.has_value() || !value.has_value() ||
        !script.has_value() || !unlocking_script.has_value() || !serialized.has_value() ||
        *input_index > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }

    const auto parameters =
        nova::chain::NetworkValidationParams(nova::consensus::RegtestNetworkParams());
    const auto transaction = nova::primitives::Transaction::Deserialize(
        *serialized, parameters.block_limits.transaction_limits);
    if (!transaction.has_value() || *input_index >= transaction->inputs.size() ||
        transaction->inputs.at(static_cast<std::size_t>(*input_index)).script_sig !=
            *unlocking_script) {
        return std::nullopt;
    }
    return SignatureAuthorizationRecord{
        *transaction,
        static_cast<std::size_t>(*input_index),
        nova::chain::Coin{{*value, *script}, 0U, false},
        *expected,
    };
}

std::optional<nova::chain::BlockValidationError>
ReplaySignatureAuthorization(const SignatureAuthorizationRecord& record)
{
    const auto parameters =
        nova::chain::NetworkValidationParams(nova::consensus::RegtestNetworkParams());
    const auto key = nova::chain::UTXOKey::FromOutPoint(
        record.transaction.inputs.at(record.input_index).previous_output);
    nova::chain::UTXOSet utxos;
    if (!utxos.AddCoin(key, record.referenced_coin)) {
        return std::nullopt;
    }
    const auto before = utxos.GetCoin(key);
    const auto before_size = utxos.size();
    if (!before.has_value()) {
        return std::nullopt;
    }

    const auto result = nova::chain::ValidateTransactionAgainstUTXOSet(record.transaction, {1U, 1U},
                                                                       parameters, utxos);
    const auto after = utxos.GetCoin(key);
    if (utxos.size() != before_size || !after.has_value() ||
        after->output.value != before->output.value ||
        after->output.script_pubkey != before->output.script_pubkey ||
        after->height != before->height || after->is_coinbase != before->is_coinbase) {
        return std::nullopt;
    }
    return result.error;
}

std::optional<std::string> SerializeTransactionHex(const nova::primitives::Transaction& transaction)
{
    const auto parameters =
        nova::chain::NetworkValidationParams(nova::consensus::RegtestNetworkParams());
    nova::primitives::BinaryWriter writer;
    if (!transaction.Serialize(writer, parameters.block_limits.transaction_limits)) {
        return std::nullopt;
    }
    return HexText(writer.bytes());
}

Record SignatureAuthorizationFixture()
{
    return {
        {"expected_result", "accept"},
        {"id", "signature-authorization-in-memory-001"},
        {"input_index", "0"},
        {"kind", "signature_authorization"},
        {"network", "REGTEST"},
        {"referenced_output_script", "76a914751e76e8199196d454941c45d1b3a323f1433bd688ac"},
        {"referenced_output_value", "5000000000"},
        {"sighash_selector", "1"},
        {"source_refs", "ProtocolConformance.InMemorySignatureAuthorizationFixture"},
        {"spec_section", "8"},
        {"status", "TEST_ONLY"},
        {"transaction",
         "01000000010000000000000000000000000000000000000000000000000000000000000011000000006a47304"
         "402205c863e12817dc3b8953deeb4d0d949a42de7aaca9df8e8760d0a8a6b64faa02502200734e9f85618f5b8"
         "284e3c2ff9e9103438e919bd0fa7f0a452ceff57c8e95a3601210279be667ef9dcbbac55a06295ce870b07029"
         "bfcdb2dce28d959f2815b16f81798ffffffff0100111024010000001976a914751e76e8199196d454941c45d1"
         "b3a323f1433bd688ac00000000"},
        {"unlocking_script",
         "47304402205c863e12817dc3b8953deeb4d0d949a42de7aaca9df8e8760d0a8a6b64faa02502200734e9f8561"
         "8f5b8284e3c2ff9e9103438e919bd0fa7f0a452ceff57c8e95a3601210279be667ef9dcbbac55a06295ce870b"
         "07029bfcdb2dce28d959f2815b16f81798"},
    };
}

std::optional<Record> RecordWithUnlockingScript(Record record,
                                                std::vector<std::uint8_t> unlocking_script)
{
    const auto parsed = ParseSignatureAuthorizationRecord(record);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    auto transaction = parsed->transaction;
    transaction.inputs.at(parsed->input_index).script_sig = std::move(unlocking_script);
    const auto serialized = SerializeTransactionHex(transaction);
    if (!serialized.has_value()) {
        return std::nullopt;
    }
    record["transaction"] = *serialized;
    record["unlocking_script"] = HexText(transaction.inputs.at(parsed->input_index).script_sig);
    return record;
}

std::string EncodeRecord(const Record& record)
{
    std::ostringstream encoded;
    encoded << "NCPV1\n";
    for (const auto& [name, value] : record) {
        encoded << name << '=' << value << '\n';
    }
    encoded << ".\n";
    return encoded.str();
}

std::vector<nova::crypto::Hash256> ParseHashes(const std::string_view text)
{
    std::vector<nova::crypto::Hash256> hashes;
    std::size_t begin = 0U;
    while (begin < text.size()) {
        const auto end = text.find(',', begin);
        const auto item =
            text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
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

TEST(ProtocolConformance, SignatureAuthorizationCorpus)
{
    for (const std::string_view filename :
         {"signature_auth_valid.ncpv", "signature_auth_malformed.ncpv",
          "signature_auth_keyhash_mismatch.ncpv", "signature_auth_digest_mismatch.ncpv",
          "signature_auth_boundary.ncpv", "signature_auth_boundary_max.ncpv"}) {
        SCOPED_TRACE(filename);
        const auto record = ReadRecord(filename);
        ASSERT_TRUE(record.has_value());
        const auto bytes = ReadFile(VectorDirectory() / filename);
        EXPECT_EQ(std::string(bytes.begin(), bytes.end()), EncodeRecord(*record));
        const auto parsed = ParseSignatureAuthorizationRecord(*record);
        ASSERT_TRUE(parsed.has_value());
        const auto result = ReplaySignatureAuthorization(*parsed);
        ASSERT_TRUE(result.has_value()) << "replay failed or controlled UTXO view changed";
        EXPECT_EQ(*result, parsed->expected_result);
    }
}

TEST(ProtocolConformance, SignatureAuthorizationCorpusBindsReviewedDigest)
{
    const auto expected = nova::crypto::Hash256::FromHex(
        "5e2d72e3b19679b59812b4d7b04ac13f9e8611a0fbc9b3a912b90218ee6fd03d");
    ASSERT_TRUE(expected.has_value());
    const auto parameters =
        nova::chain::NetworkValidationParams(nova::consensus::RegtestNetworkParams());
    for (const std::string_view filename :
         {"signature_auth_valid.ncpv", "signature_auth_digest_mismatch.ncpv"}) {
        SCOPED_TRACE(filename);
        const auto record = ReadRecord(filename);
        ASSERT_TRUE(record.has_value());
        const auto parsed = ParseSignatureAuthorizationRecord(*record);
        ASSERT_TRUE(parsed.has_value());
        const auto digest = nova::chain::ComputeSignatureHash(
            parsed->transaction, parsed->input_index, parsed->referenced_coin.output.script_pubkey,
            parameters.block_limits.transaction_limits, parameters.sighash_all);
        ASSERT_TRUE(digest.has_value());
        if (filename == "signature_auth_valid.ncpv") {
            EXPECT_EQ(*digest, *expected);
            EXPECT_EQ(Field(*record, "transaction"),
                      Field(SignatureAuthorizationFixture(), "transaction"));
        } else {
            EXPECT_NE(*digest, *expected);
            EXPECT_EQ(Field(*record, "unlocking_script"),
                      Field(SignatureAuthorizationFixture(), "unlocking_script"));
        }
    }
}

TEST(ProtocolConformance, SignatureAuthorizationCorpusPushBoundaries)
{
    for (const std::string_view filename :
         {"signature_auth_boundary.ncpv", "signature_auth_boundary_max.ncpv"}) {
        SCOPED_TRACE(filename);
        const auto record = ReadRecord(filename);
        ASSERT_TRUE(record.has_value());
        const auto parsed = ParseSignatureAuthorizationRecord(*record);
        ASSERT_TRUE(parsed.has_value());
        const auto& script = parsed->transaction.inputs.at(parsed->input_index).script_sig;
        const bool minimum = filename == "signature_auth_boundary.ncpv";
        const std::size_t push_size = minimum ? 9U : 73U;
        ASSERT_EQ(script.size(), push_size + 35U);
        EXPECT_EQ(script.front(), push_size);
        EXPECT_EQ(script.at(push_size), 1U);
        EXPECT_EQ(script.at(push_size + 1U), 33U);
        EXPECT_TRUE(
            nova::crypto::PublicKey::FromCompressed(std::span<const std::uint8_t>{script}.last(33U))
                .has_value());
        // Length admission is not signature validity. The upper fixture is high-S.
        const auto signature = nova::crypto::Signature::FromDer(
            std::span<const std::uint8_t>{script}.subspan(1U, push_size - 1U));
        EXPECT_EQ(signature.has_value(), minimum);
        EXPECT_EQ(parsed->expected_result,
                  minimum ? nova::chain::BlockValidationError::kInvalidSignature
                          : nova::chain::BlockValidationError::kMalformedUnlockingScript);
    }
}

TEST(ProtocolConformance, SignatureAuthorizationSchemaRejectsUnknownAndMissingFields)
{
    const auto fixture = SignatureAuthorizationFixture();
    std::istringstream valid_input{EncodeRecord(fixture)};
    const auto valid_record = ParseRecord(valid_input);
    ASSERT_TRUE(valid_record.has_value());
    const auto valid = ParseSignatureAuthorizationRecord(*valid_record);
    ASSERT_TRUE(valid.has_value());
    EXPECT_EQ(valid->input_index, 0U);
    EXPECT_EQ(valid->transaction.inputs.at(0U).script_sig.size(), 106U);
    EXPECT_EQ(valid->transaction.inputs.at(0U).script_sig.back(), 0x98U);

    auto unknown_field = fixture;
    unknown_field.emplace("unexpected", "value");
    std::istringstream unknown_input{EncodeRecord(unknown_field)};
    const auto parsed_unknown = ParseRecord(unknown_input);
    ASSERT_TRUE(parsed_unknown.has_value());
    EXPECT_FALSE(ParseSignatureAuthorizationRecord(*parsed_unknown).has_value());

    auto missing_field = fixture;
    missing_field.erase("referenced_output_script");
    std::istringstream missing_input{EncodeRecord(missing_field)};
    const auto parsed_missing = ParseRecord(missing_input);
    ASSERT_TRUE(parsed_missing.has_value());
    EXPECT_FALSE(ParseSignatureAuthorizationRecord(*parsed_missing).has_value());

    auto unsupported_result = fixture;
    unsupported_result["expected_result"] = "other_error";
    std::istringstream result_input{EncodeRecord(unsupported_result)};
    const auto parsed_result = ParseRecord(result_input);
    ASSERT_TRUE(parsed_result.has_value());
    EXPECT_FALSE(ParseSignatureAuthorizationRecord(*parsed_result).has_value());

    auto noncanonical_index = fixture;
    noncanonical_index["input_index"] = "00";
    std::istringstream index_input{EncodeRecord(noncanonical_index)};
    const auto parsed_index = ParseRecord(index_input);
    ASSERT_TRUE(parsed_index.has_value());
    EXPECT_FALSE(ParseSignatureAuthorizationRecord(*parsed_index).has_value());
}

TEST(ProtocolConformance, SignatureAuthorizationReplayPreservesUtxosForEveryResult)
{
    const auto fixture = SignatureAuthorizationFixture();
    const auto accepted = ParseSignatureAuthorizationRecord(fixture);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(ReplaySignatureAuthorization(*accepted), nova::chain::BlockValidationError::kNone);

    auto malformed_script = RecordWithUnlockingScript(fixture, {0U});
    ASSERT_TRUE(malformed_script.has_value());
    (*malformed_script)["expected_result"] = "malformed_unlocking_script";
    const auto malformed = ParseSignatureAuthorizationRecord(*malformed_script);
    ASSERT_TRUE(malformed.has_value());
    EXPECT_EQ(ReplaySignatureAuthorization(*malformed),
              nova::chain::BlockValidationError::kMalformedUnlockingScript);

    auto mismatched_key = accepted->transaction.inputs.at(accepted->input_index).script_sig;
    static constexpr std::array<std::uint8_t, 33U> kOtherCompressedKey{
        0x02U, 0xC6U, 0x04U, 0x7FU, 0x94U, 0x41U, 0xEDU, 0x7DU, 0x6DU, 0x30U, 0x45U,
        0x40U, 0x6EU, 0x95U, 0xC0U, 0x7CU, 0xD8U, 0x5CU, 0x77U, 0x8EU, 0x4BU, 0x8CU,
        0xEFU, 0x3CU, 0xA7U, 0xABU, 0xACU, 0x09U, 0xB9U, 0x5CU, 0x70U, 0x9EU, 0xE5U};
    std::copy(kOtherCompressedKey.begin(), kOtherCompressedKey.end(), mismatched_key.end() - 33);
    auto invalid_key_script = RecordWithUnlockingScript(fixture, std::move(mismatched_key));
    ASSERT_TRUE(invalid_key_script.has_value());
    (*invalid_key_script)["expected_result"] = "invalid_public_key";
    const auto invalid_key = ParseSignatureAuthorizationRecord(*invalid_key_script);
    ASSERT_TRUE(invalid_key.has_value());
    EXPECT_EQ(ReplaySignatureAuthorization(*invalid_key),
              nova::chain::BlockValidationError::kInvalidPublicKey);

    auto invalid_signature_script =
        accepted->transaction.inputs.at(accepted->input_index).script_sig;
    invalid_signature_script.at(10U) ^= 0x01U;
    auto invalid_signature_record =
        RecordWithUnlockingScript(fixture, std::move(invalid_signature_script));
    ASSERT_TRUE(invalid_signature_record.has_value());
    (*invalid_signature_record)["expected_result"] = "invalid_signature";
    const auto invalid_signature = ParseSignatureAuthorizationRecord(*invalid_signature_record);
    ASSERT_TRUE(invalid_signature.has_value());
    EXPECT_EQ(ReplaySignatureAuthorization(*invalid_signature),
              nova::chain::BlockValidationError::kInvalidSignature);
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
    for (const std::string_view filename :
         {std::string_view{"hashing.ncpv"}, std::string_view{"sighash.ncpv"}}) {
        const auto record = ReadRecord(filename);
        ASSERT_TRUE(record.has_value()) << filename;
        const auto input = HexBytes(
            Field(*record, filename == std::string_view{"hashing.ncpv"} ? "input" : "preimage"));
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
    const auto actual_id =
        transaction->TxId(nova::consensus::TestnetNetworkParams().block_limits.transaction_limits);
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
    const auto compact =
        static_cast<std::uint32_t>(std::stoul(Field(*target, "bits"), nullptr, 16));
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
