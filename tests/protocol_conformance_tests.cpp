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
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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
    if (value == "unsupported_locking_script") {
        return BlockValidationError::kUnsupportedLockingScript;
    }
    if (value == "input_value_too_low") {
        return BlockValidationError::kInputValueTooLow;
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

std::optional<nova::chain::BlockValidationError> ReplaySignatureAuthorization(
    const SignatureAuthorizationRecord& record,
    const std::optional<nova::primitives::Amount> expected_fee = std::nullopt)
{
    const auto parameters =
        nova::chain::NetworkValidationParams(nova::consensus::RegtestNetworkParams());
    const auto key = nova::chain::UTXOKey::FromOutPoint(
        record.transaction.inputs.at(record.input_index).previous_output);
    nova::chain::UTXOSet utxos;
    if (!utxos.AddCoin(key, record.referenced_coin)) {
        return std::nullopt;
    }
    auto unrelated_key = key;
    unrelated_key.output_index = 1U;
    if (unrelated_key == key || !utxos.AddCoin(unrelated_key, {{1LL, {0x51U}}, 0U, false})) {
        return std::nullopt;
    }
    const auto before = utxos.GetCoin(key);
    const auto unrelated_before = utxos.GetCoin(unrelated_key);
    const auto before_size = utxos.size();
    if (!before.has_value() || !unrelated_before.has_value()) {
        return std::nullopt;
    }

    const auto result = nova::chain::ValidateTransactionAgainstUTXOSet(record.transaction, {1U, 1U},
                                                                       parameters, utxos);
    const auto after = utxos.GetCoin(key);
    const auto unrelated_after = utxos.GetCoin(unrelated_key);
    if (utxos.size() != before_size || !after.has_value() ||
        after->output.value != before->output.value ||
        after->output.script_pubkey != before->output.script_pubkey ||
        after->height != before->height || after->is_coinbase != before->is_coinbase ||
        !unrelated_after.has_value() ||
        unrelated_after->output.value != unrelated_before->output.value ||
        unrelated_after->output.script_pubkey != unrelated_before->output.script_pubkey ||
        unrelated_after->height != unrelated_before->height ||
        unrelated_after->is_coinbase != unrelated_before->is_coinbase ||
        (expected_fee.has_value() && (result.error != nova::chain::BlockValidationError::kNone ||
                                      result.fee != *expected_fee))) {
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
          "signature_auth_boundary.ncpv", "signature_auth_boundary_max.ncpv",
          "signature_auth_key_prefix.ncpv", "signature_auth_der_tag.ncpv",
          "signature_auth_trailing.ncpv", "signature_auth_push_below.ncpv",
          "signature_auth_push_above.ncpv", "signature_auth_unsupported_lock.ncpv",
          "signature_auth_amount_equal.ncpv", "signature_auth_amount_insufficient.ncpv"}) {
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

TEST(ProtocolConformance, SignatureAuthorizationPreimageAndReferencedAmount)
{
    // Independently serialized public fixture; no referenced amount field exists in v0 sighash.
    const auto preimage = HexBytes(
        "0100000001000000000000000000000000000000000000000000000000000000000000001100000000"
        "1976a914751e76e8199196d454941c45d1b3a323f1433bd688acffffffff01001110240100000019"
        "76a914751e76e8199196d454941c45d1b3a323f1433bd688ac0000000001000000");
    ASSERT_TRUE(preimage.has_value());
    const auto expected = nova::crypto::Hash256::DoubleSha256(*preimage);
    ASSERT_TRUE(expected.has_value());
    EXPECT_EQ(HexText(expected->bytes()),
              "5e2d72e3b19679b59812b4d7b04ac13f9e8611a0fbc9b3a912b90218ee6fd03d");
    const auto parameters =
        nova::chain::NetworkValidationParams(nova::consensus::RegtestNetworkParams());
    const auto fixture = SignatureAuthorizationFixture();
    for (const std::string_view filename :
         {"signature_auth_valid.ncpv", "signature_auth_amount_equal.ncpv",
          "signature_auth_amount_insufficient.ncpv"}) {
        SCOPED_TRACE(filename);
        const auto record = ReadRecord(filename);
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(Field(*record, "transaction"), Field(fixture, "transaction"));
        EXPECT_EQ(Field(*record, "unlocking_script"), Field(fixture, "unlocking_script"));
        const auto parsed = ParseSignatureAuthorizationRecord(*record);
        ASSERT_TRUE(parsed.has_value());
        const auto digest = nova::chain::ComputeSignatureHash(
            parsed->transaction, parsed->input_index, parsed->referenced_coin.output.script_pubkey,
            parameters.block_limits.transaction_limits, parameters.sighash_all);
        ASSERT_TRUE(digest.has_value());
        EXPECT_EQ(*digest, *expected);
        const auto value = parsed->referenced_coin.output.value;
        if (filename == "signature_auth_amount_insufficient.ncpv") {
            EXPECT_EQ(value, 4'899'999'999LL);
            EXPECT_EQ(ReplaySignatureAuthorization(*parsed),
                      nova::chain::BlockValidationError::kInputValueTooLow);
        } else {
            EXPECT_EQ(value,
                      filename == "signature_auth_valid.ncpv" ? 5'000'000'000LL : 4'900'000'000LL);
            EXPECT_EQ(ReplaySignatureAuthorization(*parsed, value - 4'900'000'000LL),
                      nova::chain::BlockValidationError::kNone);
        }
    }
}

TEST(ProtocolConformance, SignatureAuthorizationRejectsAdjacentPushLengths)
{
    for (const auto& [filename, push] : std::array<std::pair<std::string_view, std::size_t>, 2>{
             {{"signature_auth_push_below.ncpv", 8U}, {"signature_auth_push_above.ncpv", 74U}}}) {
        const auto record = ReadRecord(filename);
        ASSERT_TRUE(record.has_value());
        const auto parsed = ParseSignatureAuthorizationRecord(*record);
        ASSERT_TRUE(parsed.has_value());
        const auto& script = parsed->transaction.inputs.at(parsed->input_index).script_sig;
        ASSERT_EQ(script.size(), push + 35U);
        EXPECT_EQ(script.front(), push);
        EXPECT_EQ(script.at(push + 1U), 33U);
        EXPECT_EQ(ReplaySignatureAuthorization(*parsed),
                  nova::chain::BlockValidationError::kMalformedUnlockingScript);
    }
}

TEST(ProtocolConformance, SignatureAuthorizationBindsSingleFaultMutations)
{
    const auto original = HexBytes(Field(SignatureAuthorizationFixture(), "unlocking_script"));
    ASSERT_TRUE(original.has_value());
    for (const std::string_view filename :
         {"signature_auth_key_prefix.ncpv", "signature_auth_der_tag.ncpv",
          "signature_auth_trailing.ncpv", "signature_auth_unsupported_lock.ncpv"}) {
        const auto record = ReadRecord(filename);
        ASSERT_TRUE(record.has_value());
        const auto parsed = ParseSignatureAuthorizationRecord(*record);
        ASSERT_TRUE(parsed.has_value());
        auto expected_script = *original;
        if (filename == "signature_auth_key_prefix.ncpv") {
            expected_script.at(expected_script.size() - 33U) = 4U;
        } else if (filename == "signature_auth_der_tag.ncpv") {
            expected_script.at(1U) = 0x31U;
        } else if (filename == "signature_auth_trailing.ncpv") {
            expected_script.push_back(0U);
        } else {
            EXPECT_EQ(parsed->referenced_coin.output.script_pubkey,
                      (std::vector<std::uint8_t>{0x51U}));
        }
        EXPECT_EQ(parsed->transaction.inputs.at(parsed->input_index).script_sig, expected_script);
        auto restored = parsed->transaction;
        restored.inputs.at(parsed->input_index).script_sig = *original;
        EXPECT_EQ(SerializeTransactionHex(restored),
                  Field(SignatureAuthorizationFixture(), "transaction"));
        EXPECT_EQ(ReplaySignatureAuthorization(*parsed), parsed->expected_result);
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

bool PureVectorSchema(const Record& record)
{
    const auto kind = record.find("kind");
    if (kind == record.end())
        return false;
    std::vector<std::string_view> names{"expected_result", "id",           "kind",  "network",
                                        "source_refs",     "spec_section", "status"};
    if (kind->second == "sighash_context") {
        for (const auto name : {"expected_digest", "expected_preimage", "input_index",
                                "referenced_output_script", "sighash_selector", "transaction"})
            names.emplace_back(name);
    } else if (kind->second == "compact_target") {
        names.emplace_back("bits");
        names.emplace_back("target");
    } else if (kind->second == "block_work") {
        names.emplace_back("target");
        names.emplace_back("expected_work");
    } else if (kind->second == "pow_compare") {
        names.emplace_back("target");
        names.emplace_back("block_hash");
    } else {
        return false;
    }
    if (record.size() != names.size())
        return false;
    for (const auto name : names) {
        const auto found = record.find(std::string{name});
        if (found == record.end() || found->second.empty())
            return false;
    }
    return record.at("network") == "REGTEST" && record.at("status") == "TEST_ONLY" &&
           record.at("spec_section") == (kind->second == "sighash_context" ? "8"
                                         : kind->second == "block_work"    ? "19"
                                                                           : "14");
}

bool ReplayPureVector(const Record& record)
{
    if (!PureVectorSchema(record))
        return false;
    const auto& kind = record.at("kind");
    const auto& result = record.at("expected_result");
    const auto& network = nova::consensus::RegtestNetworkParams();
    if (kind == "sighash_context") {
        const auto index = ParseCanonicalUnsigned<std::uint64_t>(record.at("input_index"));
        const auto selector = ParseCanonicalUnsigned<std::uint32_t>(record.at("sighash_selector"));
        const auto bytes = HexBytes(record.at("transaction"));
        const auto script = HexBytes(record.at("referenced_output_script"));
        if (!index || *index > std::numeric_limits<std::size_t>::max() || !selector || !bytes ||
            !script)
            return false;
        const auto& limits = network.block_limits.transaction_limits;
        const auto transaction = nova::primitives::Transaction::Deserialize(*bytes, limits);
        if (!transaction)
            return false;
        const auto digest = nova::chain::ComputeSignatureHash(
            *transaction, static_cast<std::size_t>(*index), *script, limits, *selector);
        if (result == "unavailable")
            return !digest && record.at("expected_digest") == "none" &&
                   record.at("expected_preimage") == "none";
        if (result != "accept" || !digest || *index >= transaction->inputs.size())
            return false;
        const auto expected = HexBytes(record.at("expected_digest"));
        const auto preimage = HexBytes(record.at("expected_preimage"));
        if (!expected || expected->size() != 32U || !preimage)
            return false;
        auto unsigned_transaction = *transaction;
        for (auto& input : unsigned_transaction.inputs)
            input.script_sig.clear();
        unsigned_transaction.inputs.at(static_cast<std::size_t>(*index)).script_sig = *script;
        nova::primitives::BinaryWriter writer;
        if (!unsigned_transaction.Serialize(writer, limits) || !writer.WriteU32(*selector))
            return false;
        return HexText(writer.bytes()) == record.at("expected_preimage") &&
               HexText(digest->bytes()) == record.at("expected_digest");
    }
    if (kind == "compact_target") {
        const auto bytes = HexBytes(record.at("bits"));
        if (!bytes || bytes->size() != 4U)
            return false;
        std::uint32_t bits = 0U;
        for (const auto byte : *bytes)
            bits = (bits << 8U) | byte;
        const auto decoded = nova::consensus::TargetFromCompact(bits, network.pow);
        using Error = nova::consensus::TargetError;
        std::optional<Error> expected;
        for (const auto& [name, error] :
             {std::pair{"accept", Error::kNone}, std::pair{"negative", Error::kNegative},
              std::pair{"zero", Error::kZero}, std::pair{"overflow", Error::kOverflow},
              std::pair{"noncanonical", Error::kNonCanonical},
              std::pair{"above_pow_limit", Error::kAbovePowLimit}}) {
            if (result == name)
                expected = error;
        }
        if (!expected || decoded.error != *expected)
            return false;
        if (*expected != Error::kNone)
            return !decoded.target && record.at("target") == "none";
        const auto target_bytes = HexBytes(record.at("target"));
        if (!target_bytes || target_bytes->size() != 32U || !decoded.target)
            return false;
        return HexText(decoded.target->bytes()) == record.at("target") &&
               nova::consensus::CompactFromTarget(*decoded.target) == bits;
    }
    const auto bytes = HexBytes(record.at("target"));
    if (!bytes || bytes->size() != 32U)
        return false;
    const auto target = nova::consensus::Target256::FromBigEndian(*bytes);
    if (!target)
        return false;
    if (kind == "block_work") {
        const auto work = nova::consensus::CalculateWork(*target);
        if (result == "unavailable")
            return !work && record.at("expected_work") == "none";
        const auto expected = HexBytes(record.at("expected_work"));
        return result == "accept" && work && expected && expected->size() == 32U &&
               HexText(work->bytes()) == record.at("expected_work");
    }
    const auto hash_bytes = HexBytes(record.at("block_hash"));
    if (!hash_bytes || hash_bytes->size() != 32U || (result != "accept" && result != "reject"))
        return false;
    const auto hash = nova::crypto::Hash256::FromHex(record.at("block_hash"));
    return hash && nova::consensus::CheckProofOfWork(*hash, *target) == (result == "accept");
}

bool IsCanonicalUnsignedDecimal(const std::string_view text)
{
    return !text.empty() && (text.size() == 1U || text.front() != '0') &&
           std::all_of(text.begin(), text.end(),
                       [](const char value) { return value >= '0' && value <= '9'; });
}

std::optional<std::uint32_t> ParseCanonicalCompact(const std::string_view text)
{
    if (text.size() != 8U || !std::all_of(text.begin(), text.end(), [](const char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        })) {
        return std::nullopt;
    }
    std::uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

bool DifficultyVectorSchema(const Record& record)
{
    static constexpr std::array<std::string_view, 32U> kFields{"candidate_bits",
                                                               "candidate_delay_seconds",
                                                               "candidate_height",
                                                               "candidate_time",
                                                               "clamped_elapsed_seconds",
                                                               "expected_bits",
                                                               "expected_elapsed_seconds",
                                                               "expected_result",
                                                               "genesis_time",
                                                               "id",
                                                               "kind",
                                                               "network",
                                                               "parent_bits",
                                                               "parent_delta_seconds",
                                                               "parent_height",
                                                               "parent_time",
                                                               "pow_limit_bits",
                                                               "prefix_blocks",
                                                               "prefix_spacing_seconds",
                                                               "represented_target",
                                                               "retarget_bits",
                                                               "retarget_interval",
                                                               "scale_product",
                                                               "scale_quotient",
                                                               "scale_remainder",
                                                               "scenario",
                                                               "source_refs",
                                                               "spec_section",
                                                               "status",
                                                               "target_spacing_seconds",
                                                               "target_timespan_seconds",
                                                               "truncation_loss"};
    const auto scenario_it = record.find("scenario");
    if (scenario_it == record.end()) {
        return false;
    }
    static constexpr std::array<std::string_view, 7U> kUpperHistoryFields{
        "first_interval_height",
        "first_interval_time",
        "history_delay_height",
        "history_delay_seconds",
        "history_final_parent_delta_seconds",
        "history_post_delay_blocks",
        "history_post_delay_spacing_seconds"};
    const bool has_upper_history = scenario_it->second == "testnet_retarget_upper_clamp";
    if (record.size() != kFields.size() + (has_upper_history ? kUpperHistoryFields.size() : 0U)) {
        return false;
    }
    for (const auto name : kFields) {
        const auto found = record.find(std::string{name});
        if (found == record.end() || found->second.empty()) {
            return false;
        }
    }
    if (has_upper_history) {
        for (const auto name : kUpperHistoryFields) {
            const auto found = record.find(std::string{name});
            if (found == record.end() ||
                !ParseCanonicalUnsigned<std::uint32_t>(found->second).has_value()) {
                return false;
            }
        }
    }
    if (record.at("kind") != "difficulty_adjustment" ||
        record.at("source_refs") != "ChainState.ExpectedBits,ChainState.AcceptBlock" ||
        record.at("spec_section") != "15" || record.at("status") != "TEST_ONLY" ||
        (record.at("expected_result") != "accept" &&
         record.at("expected_result") != "unexpected_bits")) {
        return false;
    }
    const auto network = record.at("network");
    const auto scenario = record.at("scenario");
    if (!((network == "TESTNET" &&
           (scenario == "testnet_retarget" || scenario == "testnet_minimum_delay" ||
            scenario == "testnet_retarget_upper_pow_limit_cap" || has_upper_history)) ||
          (network == "REGTEST" && scenario == "regtest_no_retarget"))) {
        return false;
    }
    for (const auto name :
         {"candidate_delay_seconds", "candidate_height", "candidate_time", "genesis_time",
          "parent_height", "parent_time", "prefix_blocks", "retarget_interval",
          "target_spacing_seconds", "target_timespan_seconds"}) {
        if (!ParseCanonicalUnsigned<std::uint32_t>(record.at(name)).has_value()) {
            return false;
        }
    }
    for (const auto name : {"parent_delta_seconds", "prefix_spacing_seconds",
                            "expected_elapsed_seconds", "clamped_elapsed_seconds"}) {
        if (record.at(name) != "none" &&
            !ParseCanonicalUnsigned<std::uint32_t>(record.at(name)).has_value()) {
            return false;
        }
    }
    for (const auto name :
         {"scale_product", "scale_quotient", "scale_remainder", "truncation_loss"}) {
        if (record.at(name) != "none" && !IsCanonicalUnsignedDecimal(record.at(name))) {
            return false;
        }
    }
    for (const auto name : {"candidate_bits", "expected_bits", "parent_bits", "pow_limit_bits"}) {
        if (!ParseCanonicalCompact(record.at(name)).has_value()) {
            return false;
        }
    }
    const auto parent_height = ParseCanonicalUnsigned<std::uint32_t>(record.at("parent_height"));
    const auto candidate_height =
        ParseCanonicalUnsigned<std::uint32_t>(record.at("candidate_height"));
    const auto parent_time = ParseCanonicalUnsigned<std::uint32_t>(record.at("parent_time"));
    const auto candidate_time = ParseCanonicalUnsigned<std::uint32_t>(record.at("candidate_time"));
    const auto candidate_delay =
        ParseCanonicalUnsigned<std::uint32_t>(record.at("candidate_delay_seconds"));
    const auto parent_bits = ParseCanonicalCompact(record.at("parent_bits"));
    const auto expected_bits = ParseCanonicalCompact(record.at("expected_bits"));
    const auto candidate_bits = ParseCanonicalCompact(record.at("candidate_bits"));
    if (!parent_height || !candidate_height || *candidate_height == 0U ||
        *parent_height != *candidate_height - 1U || !parent_time || !candidate_time ||
        !candidate_delay || *candidate_delay == 0U || *candidate_time <= *parent_time ||
        *candidate_time - *parent_time != *candidate_delay || !parent_bits || !expected_bits ||
        !candidate_bits ||
        ((record.at("expected_result") == "accept") != (*candidate_bits == *expected_bits))) {
        return false;
    }
    if (record.at("retarget_bits") != "none" &&
        !ParseCanonicalCompact(record.at("retarget_bits")).has_value()) {
        return false;
    }
    if (record.at("represented_target") != "none" &&
        (record.at("represented_target").size() != 64U ||
         !HexBytes(record.at("represented_target")).has_value())) {
        return false;
    }
    for (const auto name :
         {"parent_delta_seconds", "prefix_spacing_seconds", "expected_elapsed_seconds",
          "clamped_elapsed_seconds", "scale_product", "scale_quotient", "scale_remainder",
          "truncation_loss", "represented_target", "retarget_bits"}) {
        const bool should_be_none = network == "REGTEST";
        if ((record.at(name) == "none") != should_be_none) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> DifficultyCoinbaseHeightScript(const std::uint32_t height,
                                                         const std::uint32_t timestamp,
                                                         const std::uint32_t bits)
{
    std::array<std::uint8_t, 5U> encoded{};
    std::size_t encoded_size = 0U;
    auto remaining = height;
    while (remaining != 0U) {
        encoded.at(encoded_size++) = static_cast<std::uint8_t>(remaining & 0xFFU);
        remaining >>= 8U;
    }
    if (encoded_size != 0U && (encoded.at(encoded_size - 1U) & 0x80U) != 0U) {
        encoded.at(encoded_size++) = 0U;
    }
    std::vector<std::uint8_t> script;
    script.reserve(encoded_size + 5U);
    script.push_back(static_cast<std::uint8_t>(encoded_size));
    script.insert(script.end(), encoded.begin(),
                  encoded.begin() + static_cast<std::ptrdiff_t>(encoded_size));
    for (std::size_t index = 0U; index < sizeof(timestamp); ++index) {
        script.push_back(static_cast<std::uint8_t>(timestamp >> (8U * index)));
    }
    for (std::size_t index = 0U; index < sizeof(bits); ++index) {
        script.push_back(static_cast<std::uint8_t>(bits >> (8U * index)));
    }
    return script;
}

std::optional<nova::primitives::Block>
MakeDifficultyBlock(const nova::crypto::Hash256& parent, const std::uint32_t height,
                    const std::uint32_t time, const std::uint32_t bits,
                    const nova::consensus::NetworkParams& network, const bool mine)
{
    const auto subsidy = nova::consensus::GetBlockSubsidy(height, network.monetary);
    const auto target = nova::consensus::TargetFromCompact(bits, network.pow);
    if (!subsidy.has_value() || !target.target.has_value()) {
        return std::nullopt;
    }
    nova::primitives::Transaction coinbase{
        1,
        {nova::primitives::TxInput{
            nova::primitives::OutPoint{nova::crypto::Hash256{}, 0xFFFF'FFFFU},
            DifficultyCoinbaseHeightScript(height, time, bits), 0U}},
        {nova::primitives::TxOutput{*subsidy, {0x51U}}},
        0U};
    nova::primitives::Block block{{1, parent, nova::crypto::Hash256{}, time, bits, 0U},
                                  {std::move(coinbase)}};
    const auto merkle = nova::primitives::ComputeMerkleRoot(
        block.transactions, network.block_limits.transaction_limits);
    if (!merkle.has_value()) {
        return std::nullopt;
    }
    block.header.merkle_root = *merkle;
    if (!mine) {
        return block;
    }
    for (std::uint32_t nonce = 0U; nonce < 65'536U; ++nonce) {
        block.header.nonce = nonce;
        const auto hash = nova::primitives::ComputeBlockHash(block.header);
        if (hash.has_value() && nova::consensus::CheckProofOfWork(*hash, *target.target)) {
            return block;
        }
    }
    return std::nullopt;
}

bool SameCoin(const std::optional<nova::chain::Coin>& left,
              const std::optional<nova::chain::Coin>& right)
{
    if (left.has_value() != right.has_value()) {
        return false;
    }
    return !left.has_value() ||
           (left->output.value == right->output.value &&
            left->output.script_pubkey == right->output.script_pubkey &&
            left->height == right->height && left->is_coinbase == right->is_coinbase);
}

TEST(ProtocolConformance, PureConsensusVectorTriads)
{
    std::size_t count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator{VectorDirectory()}) {
        if (!entry.path().filename().string().starts_with("sighash_context_") &&
            !entry.path().filename().string().starts_with("compact_context_") &&
            !entry.path().filename().string().starts_with("work_context_") &&
            !entry.path().filename().string().starts_with("pow_context_"))
            continue;
        SCOPED_TRACE(entry.path().filename().string());
        const auto record = ReadRecord(entry.path().filename().string());
        ASSERT_TRUE(record.has_value());
        const auto raw = ReadFile(entry.path());
        EXPECT_EQ(std::string(raw.begin(), raw.end()), EncodeRecord(*record));
        EXPECT_TRUE(ReplayPureVector(*record));
        ++count;
    }
    EXPECT_EQ(count, 18U);
}

TEST(ProtocolConformance, DifficultyVectorsReplayImmutableNetworkHistories)
{
    std::map<std::string, Record> vectors;
    for (const auto& entry : std::filesystem::directory_iterator{VectorDirectory()}) {
        const auto filename = entry.path().filename().string();
        if (!filename.starts_with("difficulty_") || entry.path().extension() != ".ncpv") {
            continue;
        }
        const auto record = ReadRecord(filename);
        ASSERT_TRUE(record.has_value()) << filename;
        ASSERT_TRUE(DifficultyVectorSchema(*record)) << filename;
        EXPECT_EQ(Field(*record, "id"), entry.path().stem().string());
        vectors.emplace(Field(*record, "id"), *record);
    }
    ASSERT_EQ(vectors.size(), 15U);

    const auto number = [](const Record& record, const std::string_view name) {
        return ParseCanonicalUnsigned<std::uint32_t>(Field(record, name));
    };
    const auto compact = [](const Record& record, const std::string_view name) {
        return ParseCanonicalCompact(Field(record, name));
    };
    const auto& testnet = nova::consensus::TestnetNetworkParams();
    ASSERT_FALSE(testnet.difficulty.no_retargeting);
    const auto testnet_now = std::make_shared<nova::chain::FixedValidationTimeSource>(
        testnet.genesis_block.header.time + 6'000'000U);
    auto testnet_initialized = nova::chain::NetworkChainInitializer::Create(
        nova::consensus::NetworkId::kTestnet, testnet_now);
    ASSERT_TRUE(testnet_initialized.has_value());
    auto& testnet_chain = *testnet_initialized->chain;
    auto& testnet_utxos = *testnet_initialized->utxos;
    ASSERT_EQ(testnet_chain.active_tip(), testnet.genesis_hash);
    ASSERT_EQ(testnet_chain.active_height(), 0U);

    const auto testnet_pow_bits = testnet.pow.pow_limit_compact;
    const auto prefix_record = vectors.find("difficulty_testnet_retarget_302399");
    ASSERT_NE(prefix_record, vectors.end());
    const auto prefix_blocks = number(prefix_record->second, "prefix_blocks");
    const auto prefix_spacing = number(prefix_record->second, "prefix_spacing_seconds");
    ASSERT_TRUE(prefix_blocks.has_value());
    ASSERT_TRUE(prefix_spacing.has_value());
    EXPECT_EQ(*prefix_blocks, testnet.difficulty.retarget_interval - 2U);
    EXPECT_EQ(testnet.difficulty.target_spacing_seconds, 600U);
    EXPECT_EQ(testnet.difficulty.target_timespan_seconds, 1'209'600U);
    EXPECT_EQ(*prefix_spacing, 150U);

    for (const auto& [name, record] : vectors) {
        SCOPED_TRACE(name);
        if (Field(record, "network") == "TESTNET") {
            EXPECT_EQ(ParseCanonicalUnsigned<std::uint32_t>(Field(record, "genesis_time")),
                      testnet.genesis_block.header.time);
            EXPECT_EQ(ParseCanonicalCompact(Field(record, "pow_limit_bits")), testnet_pow_bits);
            EXPECT_EQ(number(record, "retarget_interval"), testnet.difficulty.retarget_interval);
            EXPECT_EQ(number(record, "target_spacing_seconds"),
                      testnet.difficulty.target_spacing_seconds);
            EXPECT_EQ(number(record, "target_timespan_seconds"),
                      testnet.difficulty.target_timespan_seconds);
            const auto elapsed =
                ParseCanonicalUnsigned<std::uint32_t>(Field(record, "expected_elapsed_seconds"));
            ASSERT_TRUE(elapsed.has_value());
            const auto scenario = Field(record, "scenario");
            if (scenario == "testnet_retarget_upper_pow_limit_cap") {
                EXPECT_EQ(*elapsed, 4'838'401U);
                EXPECT_EQ(number(record, "clamped_elapsed_seconds"), 4'838'400U);
                EXPECT_EQ(Field(record, "scale_product"),
                          "247297131590726674254656723558732000761068757110962901669871253667342866"
                          "541497548800");
                EXPECT_EQ(Field(record, "scale_quotient"),
                          "204445379952651020382487370666941138195328006870835732200621076113874724"
                          "323328");
                EXPECT_GT(Field(record, "scale_quotient"),
                          "115792089237316195423570985008687907853269984665640564039457584007913129"
                          "639935");
                EXPECT_EQ(Field(record, "scale_remainder"), "0");
                EXPECT_EQ(Field(record, "represented_target"),
                          "70ffff0000000000000000000000000000000000000000000000000000000000");
                EXPECT_EQ(Field(record, "retarget_bits"), "2070ffff");
                EXPECT_EQ(Field(record, "truncation_loss"),
                          "153334034964488265286865528000205853646496005153126799150465807085406043"
                          "242496");
            } else if (scenario == "testnet_retarget_upper_clamp") {
                EXPECT_TRUE(*elapsed == 4'838'400U || *elapsed == 4'838'401U);
                EXPECT_EQ(number(record, "clamped_elapsed_seconds"), 4'838'400U);
                EXPECT_EQ(Field(record, "scale_product"),
                          "618242578526245253300663090464409271593972274231564872055690820430678133"
                          "12489062400");
                EXPECT_EQ(Field(record, "scale_quotient"),
                          "511113242829237147239304803624676977177556443643820165389956035408960096"
                          "82944");
                EXPECT_EQ(Field(record, "scale_remainder"), "0");
                EXPECT_EQ(Field(record, "represented_target"),
                          "70fffc0000000000000000000000000000000000000000000000000000000000");
                EXPECT_EQ(Field(record, "retarget_bits"), "2070fffc");
                EXPECT_EQ(Field(record, "truncation_loss"), "0");
                EXPECT_EQ(number(record, "first_interval_time").value() + *elapsed,
                          number(record, "parent_time").value());
            } else if (*elapsed == 302'401U) {
                EXPECT_EQ(Field(record, "clamped_elapsed_seconds"), "302401");
                EXPECT_EQ(Field(record, "scale_product"),
                          "154561218357654053036711408442634167828513461514368990633000035094779576"
                          "27524677632");
                EXPECT_EQ(Field(record, "scale_quotient"),
                          "127778785017901829560773320471754437688916552177884416859292357055869358"
                          "69316");
                EXPECT_EQ(Field(record, "scale_remainder"), "44032");
                EXPECT_EQ(Field(record, "represented_target"),
                          "1c40050000000000000000000000000000000000000000000000000000000000");
                EXPECT_EQ(Field(record, "retarget_bits"), "201c4005");
                EXPECT_EQ(Field(record, "truncation_loss"),
                          "6020581173531711987348023345677300029420039104528861003845217590652804");
            } else {
                EXPECT_TRUE(*elapsed == 302'399U || *elapsed == 302'400U);
                EXPECT_EQ(Field(record, "clamped_elapsed_seconds"), "302400");
                EXPECT_EQ(Field(record, "scale_product"),
                          "154560707244204171409160452224207500475667973194351813543669533542089291"
                          "58843596800");
                EXPECT_EQ(Field(record, "scale_quotient"),
                          "127778362470406887739054606666838211372080004294272332625388172571171702"
                          "70208");
                EXPECT_EQ(Field(record, "scale_remainder"), "0");
                EXPECT_EQ(Field(record, "represented_target"),
                          "1c3fff0000000000000000000000000000000000000000000000000000000000");
                EXPECT_EQ(Field(record, "retarget_bits"), "201c3fff");
                EXPECT_EQ(Field(record, "truncation_loss"),
                          "5176309760092922840576066896707769089338331729127789916371893167849472");
            }
        }
    }

    std::vector<nova::primitives::Block> accepted_testnet_blocks{testnet.genesis_block};
    auto prefix_parent = testnet.genesis_hash;
    auto prefix_time = testnet.genesis_block.header.time;
    for (std::uint32_t height = 1U; height <= *prefix_blocks; ++height) {
        prefix_time += *prefix_spacing;
        auto block = MakeDifficultyBlock(prefix_parent, height, prefix_time, testnet_pow_bits,
                                         testnet, true);
        ASSERT_TRUE(block.has_value()) << "TESTNET prefix height " << height;
        const auto hash = nova::primitives::ComputeBlockHash(block->header);
        ASSERT_TRUE(hash.has_value());
        const auto result = testnet_chain.AcceptBlock(*block);
        ASSERT_EQ(result.error, nova::chain::ChainError::kNone)
            << "TESTNET prefix height " << height;
        EXPECT_EQ(result.block_hash, hash);
        accepted_testnet_blocks.push_back(*block);
        prefix_parent = *hash;
    }
    ASSERT_EQ(testnet_chain.active_height(), *prefix_blocks);

    std::map<std::uint32_t, nova::primitives::Block> boundary_parents;
    for (const auto delta : {299U, 300U, 301U}) {
        const auto time = prefix_time + delta;
        auto block = MakeDifficultyBlock(prefix_parent, *prefix_blocks + 1U, time, testnet_pow_bits,
                                         testnet, true);
        ASSERT_TRUE(block.has_value());
        const auto hash = nova::primitives::ComputeBlockHash(block->header);
        ASSERT_TRUE(hash.has_value());
        const auto result = testnet_chain.AcceptBlock(*block);
        ASSERT_EQ(result.error, nova::chain::ChainError::kNone) << "parent delta " << delta;
        const auto parent_index = testnet_chain.GetBlockIndex(*hash);
        ASSERT_TRUE(parent_index.has_value());
        EXPECT_EQ(parent_index->height, *prefix_blocks + 1U);
        accepted_testnet_blocks.push_back(*block);
        boundary_parents.emplace(delta, std::move(*block));
    }

    const auto upper_cap_parent_record =
        vectors.find("difficulty_testnet_retarget_upper_pow_limit_cap");
    ASSERT_NE(upper_cap_parent_record, vectors.end());
    const auto upper_cap_parent_delta =
        number(upper_cap_parent_record->second, "parent_delta_seconds");
    const auto upper_cap_parent_time = number(upper_cap_parent_record->second, "parent_time");
    ASSERT_TRUE(upper_cap_parent_delta.has_value());
    ASSERT_TRUE(upper_cap_parent_time.has_value());
    EXPECT_EQ(prefix_time + *upper_cap_parent_delta, *upper_cap_parent_time);
    auto upper_cap_parent =
        MakeDifficultyBlock(prefix_parent, *prefix_blocks + 1U, *upper_cap_parent_time,
                            testnet_pow_bits, testnet, true);
    ASSERT_TRUE(upper_cap_parent.has_value());
    const auto upper_cap_parent_hash = nova::primitives::ComputeBlockHash(upper_cap_parent->header);
    ASSERT_TRUE(upper_cap_parent_hash.has_value());
    const auto upper_cap_parent_result = testnet_chain.AcceptBlock(*upper_cap_parent);
    ASSERT_EQ(upper_cap_parent_result.error, nova::chain::ChainError::kNone);
    const auto upper_cap_parent_index = testnet_chain.GetBlockIndex(*upper_cap_parent_hash);
    ASSERT_TRUE(upper_cap_parent_index.has_value());
    EXPECT_EQ(upper_cap_parent_index->height, *prefix_blocks + 1U);
    EXPECT_EQ(upper_cap_parent_index->expected_bits, testnet_pow_bits);
    accepted_testnet_blocks.push_back(*upper_cap_parent);
    boundary_parents.emplace(*upper_cap_parent_delta, *upper_cap_parent);

    std::map<std::uint32_t, nova::crypto::Hash256> retarget_children;

    const auto assert_rejection_is_atomic =
        [](nova::chain::ChainState& chain, nova::chain::UTXOSet& utxos,
           const nova::consensus::NetworkParams& network,
           const std::vector<nova::primitives::Block>& accepted_blocks,
           const nova::primitives::Block& rejected, const std::string_view label) {
            SCOPED_TRACE(label);
            const auto tip = chain.active_tip();
            const auto height = chain.active_height();
            const auto known_count = chain.known_block_count();
            const auto utxo_size = utxos.size();
            const auto active_before = chain.GetActiveBlockIndexes();
            ASSERT_TRUE(active_before.has_value());
            std::vector<nova::chain::BlockIndex> indexes_before;
            indexes_before.reserve(accepted_blocks.size());
            std::vector<std::optional<nova::chain::Coin>> coins_before;
            coins_before.reserve(accepted_blocks.size());
            for (const auto& block : accepted_blocks) {
                const auto block_hash = nova::primitives::ComputeBlockHash(block.header);
                ASSERT_TRUE(block_hash.has_value());
                const auto index = chain.GetBlockIndex(*block_hash);
                ASSERT_TRUE(index.has_value());
                indexes_before.push_back(*index);
                for (const auto& transaction : block.transactions) {
                    const auto txid = transaction.TxId(network.block_limits.transaction_limits);
                    ASSERT_TRUE(txid.has_value());
                    coins_before.push_back(utxos.GetCoin(nova::chain::UTXOKey{*txid, 0U}));
                }
            }
            const auto rejected_hash = nova::primitives::ComputeBlockHash(rejected.header);
            ASSERT_TRUE(rejected_hash.has_value());

            const auto result = chain.AcceptBlock(rejected);
            EXPECT_EQ(result.error, nova::chain::ChainError::kBlockValidationFailure);
            EXPECT_EQ(result.validation_error, nova::chain::BlockValidationError::kUnexpectedBits);
            EXPECT_EQ(chain.active_tip(), tip);
            EXPECT_EQ(chain.active_height(), height);
            EXPECT_EQ(chain.known_block_count(), known_count);
            EXPECT_EQ(utxos.size(), utxo_size);
            EXPECT_FALSE(chain.GetBlockIndex(*rejected_hash).has_value());
            EXPECT_FALSE(utxos.HaveCoin(
                nova::chain::UTXOKey{rejected.transactions.front()
                                         .TxId(network.block_limits.transaction_limits)
                                         .value_or(nova::crypto::Hash256{}),
                                     0U}));
            const auto active_after = chain.GetActiveBlockIndexes();
            ASSERT_TRUE(active_after.has_value());
            ASSERT_EQ(active_after->size(), active_before->size());
            for (std::size_t index = 0U; index < active_before->size(); ++index) {
                EXPECT_EQ(active_after->at(index).hash, active_before->at(index).hash);
                EXPECT_EQ(active_after->at(index).height, active_before->at(index).height);
                EXPECT_EQ(active_after->at(index).expected_bits,
                          active_before->at(index).expected_bits);
                EXPECT_EQ(active_after->at(index).chain_work, active_before->at(index).chain_work);
                EXPECT_EQ(active_after->at(index).status, active_before->at(index).status);
            }
            for (const auto& previous : indexes_before) {
                const auto current = chain.GetBlockIndex(previous.hash);
                ASSERT_TRUE(current.has_value());
                EXPECT_EQ(current->previous_hash, previous.previous_hash);
                EXPECT_EQ(current->height, previous.height);
                EXPECT_EQ(current->time, previous.time);
                EXPECT_EQ(current->expected_bits, previous.expected_bits);
                EXPECT_EQ(current->target, previous.target);
                EXPECT_EQ(current->per_block_work, previous.per_block_work);
                EXPECT_EQ(current->chain_work, previous.chain_work);
                EXPECT_EQ(current->status, previous.status);
                EXPECT_EQ(current->validation_time, previous.validation_time);
                ASSERT_EQ(current->block.has_value(), previous.block.has_value());
                if (previous.block.has_value()) {
                    EXPECT_EQ(current->block->header.version, previous.block->header.version);
                    EXPECT_EQ(current->block->header.previous_block_id,
                              previous.block->header.previous_block_id);
                    EXPECT_EQ(current->block->header.merkle_root,
                              previous.block->header.merkle_root);
                    EXPECT_EQ(current->block->header.time, previous.block->header.time);
                    EXPECT_EQ(current->block->header.bits, previous.block->header.bits);
                    EXPECT_EQ(current->block->header.nonce, previous.block->header.nonce);
                    ASSERT_EQ(current->block->transactions.size(),
                              previous.block->transactions.size());
                    for (std::size_t transaction = 0U;
                         transaction < current->block->transactions.size(); ++transaction) {
                        EXPECT_EQ(current->block->transactions.at(transaction)
                                      .TxId(network.block_limits.transaction_limits),
                                  previous.block->transactions.at(transaction)
                                      .TxId(network.block_limits.transaction_limits));
                    }
                }
            }
            std::size_t coin_index = 0U;
            for (const auto& block : accepted_blocks) {
                for (const auto& transaction : block.transactions) {
                    const auto txid = transaction.TxId(network.block_limits.transaction_limits);
                    EXPECT_TRUE(txid.has_value());
                    if (txid.has_value()) {
                        EXPECT_TRUE(SameCoin(coins_before.at(coin_index),
                                             utxos.GetCoin(nova::chain::UTXOKey{*txid, 0U})));
                    }
                    ++coin_index;
                }
            }
        };

    const auto make_testnet_candidate = [&](const Record& record, const bool mine) {
        const auto parent_delta = number(record, "parent_delta_seconds");
        const auto height = number(record, "candidate_height");
        const auto time = number(record, "candidate_time");
        const auto bits = compact(record, "candidate_bits");
        if (!parent_delta || !height || !time || !bits) {
            return std::optional<nova::primitives::Block>{};
        }
        std::optional<nova::crypto::Hash256> parent_hash;
        if (*height == testnet.difficulty.retarget_interval + 1U) {
            const auto found = retarget_children.find(*parent_delta);
            if (found != retarget_children.end()) {
                parent_hash = found->second;
            }
        } else {
            const auto parent = boundary_parents.find(*parent_delta);
            if (parent != boundary_parents.end()) {
                parent_hash = nova::primitives::ComputeBlockHash(parent->second.header);
            }
        }
        if (!parent_hash) {
            return std::optional<nova::primitives::Block>{};
        }
        return MakeDifficultyBlock(*parent_hash, *height, *time, *bits, testnet, mine);
    };
    const auto assert_testnet_record_parent = [&](const Record& record,
                                                  const nova::primitives::Block& candidate) {
        const auto parent = testnet_chain.GetBlockIndex(candidate.header.previous_block_id);
        ASSERT_TRUE(parent.has_value());
        EXPECT_EQ(number(record, "parent_height"), parent->height);
        EXPECT_EQ(number(record, "parent_time"), parent->time);
        EXPECT_EQ(compact(record, "parent_bits"), parent->expected_bits);
    };

    for (const auto delta : {299U, 300U, 301U}) {
        const auto suffix = prefix_time - testnet.genesis_block.header.time + delta;
        const auto name = "difficulty_testnet_retarget_" + std::to_string(suffix);
        const auto found = vectors.find(name);
        ASSERT_NE(found, vectors.end()) << name;
        const auto& record = found->second;
        const auto expected_bits = compact(record, "expected_bits");
        const auto candidate_bits = compact(record, "candidate_bits");
        const auto recorded_parent_bits = compact(record, "parent_bits");
        const auto retarget_bits = compact(record, "retarget_bits");
        ASSERT_TRUE(expected_bits.has_value());
        ASSERT_TRUE(candidate_bits.has_value());
        ASSERT_TRUE(recorded_parent_bits.has_value());
        ASSERT_TRUE(retarget_bits.has_value());
        EXPECT_EQ(number(record, "parent_time"), prefix_time + delta);
        EXPECT_EQ(number(record, "candidate_time"), prefix_time + delta + 1U);
        EXPECT_EQ(number(record, "candidate_delay_seconds"), 1U);
        EXPECT_EQ(number(record, "parent_height"), *prefix_blocks + 1U);
        EXPECT_EQ(number(record, "candidate_height"), testnet.difficulty.retarget_interval);
        EXPECT_EQ(*recorded_parent_bits, testnet_pow_bits);
        EXPECT_EQ(ParseCanonicalUnsigned<std::uint32_t>(Field(record, "expected_elapsed_seconds")),
                  number(record, "parent_time").value() - testnet.genesis_block.header.time);
        EXPECT_EQ(
            ParseCanonicalUnsigned<std::uint32_t>(Field(record, "clamped_elapsed_seconds")),
            std::max(testnet.difficulty.target_timespan_seconds / 4U,
                     number(record, "parent_time").value() - testnet.genesis_block.header.time));
        EXPECT_EQ(Field(record, "expected_result"), "accept");
        EXPECT_EQ(*candidate_bits, *expected_bits);
        EXPECT_EQ(*retarget_bits, *expected_bits);
        auto candidate = make_testnet_candidate(record, true);
        ASSERT_TRUE(candidate.has_value()) << name;
        assert_testnet_record_parent(record, *candidate);
        const auto candidate_hash = nova::primitives::ComputeBlockHash(candidate->header);
        ASSERT_TRUE(candidate_hash.has_value());
        const auto accepted = testnet_chain.AcceptBlock(*candidate);
        ASSERT_EQ(accepted.error, nova::chain::ChainError::kNone) << name;
        EXPECT_EQ(accepted.block_hash, candidate_hash);
        const auto index = testnet_chain.GetBlockIndex(*candidate_hash);
        ASSERT_TRUE(index.has_value());
        EXPECT_EQ(index->expected_bits, *expected_bits);
        accepted_testnet_blocks.push_back(*candidate);
        retarget_children.emplace(delta, *candidate_hash);
    }

    const auto wrong_retarget = vectors.find("difficulty_testnet_retarget_302400_wrong_bits");
    ASSERT_NE(wrong_retarget, vectors.end());
    const auto valid_retarget = vectors.find("difficulty_testnet_retarget_302400");
    ASSERT_NE(valid_retarget, vectors.end());
    EXPECT_EQ(Field(wrong_retarget->second, "expected_result"), "unexpected_bits");
    EXPECT_EQ(compact(wrong_retarget->second, "expected_bits"),
              compact(valid_retarget->second, "expected_bits"));
    EXPECT_NE(compact(wrong_retarget->second, "candidate_bits"),
              compact(wrong_retarget->second, "expected_bits"));
    auto wrong_retarget_block = make_testnet_candidate(wrong_retarget->second, false);
    ASSERT_TRUE(wrong_retarget_block.has_value());
    assert_testnet_record_parent(wrong_retarget->second, *wrong_retarget_block);
    assert_rejection_is_atomic(testnet_chain, testnet_utxos, testnet, accepted_testnet_blocks,
                               *wrong_retarget_block,
                               "retarget uses documented integer-derived bits");

    const auto& upper_cap_record = upper_cap_parent_record->second;
    const auto upper_cap_candidate = make_testnet_candidate(upper_cap_record, true);
    ASSERT_TRUE(upper_cap_candidate.has_value());
    assert_testnet_record_parent(upper_cap_record, *upper_cap_candidate);
    EXPECT_EQ(compact(upper_cap_record, "candidate_bits"), testnet_pow_bits);
    EXPECT_EQ(compact(upper_cap_record, "expected_bits"), testnet_pow_bits);
    EXPECT_EQ(number(upper_cap_record, "expected_elapsed_seconds"), 4'838'401U);
    EXPECT_EQ(number(upper_cap_record, "clamped_elapsed_seconds"), 4'838'400U);
    const auto upper_cap_candidate_hash =
        nova::primitives::ComputeBlockHash(upper_cap_candidate->header);
    ASSERT_TRUE(upper_cap_candidate_hash.has_value());
    const auto upper_cap_candidate_result = testnet_chain.AcceptBlock(*upper_cap_candidate);
    ASSERT_EQ(upper_cap_candidate_result.error, nova::chain::ChainError::kNone);
    EXPECT_EQ(upper_cap_candidate_result.block_hash, upper_cap_candidate_hash);
    const auto upper_cap_candidate_index = testnet_chain.GetBlockIndex(*upper_cap_candidate_hash);
    ASSERT_TRUE(upper_cap_candidate_index.has_value());
    EXPECT_EQ(upper_cap_candidate_index->expected_bits, testnet_pow_bits);
    accepted_testnet_blocks.push_back(*upper_cap_candidate);
    retarget_children.emplace(*upper_cap_parent_delta, *upper_cap_candidate_hash);

    const auto upper_cap_wrong =
        vectors.find("difficulty_testnet_retarget_upper_pow_limit_cap_wrong_bits");
    ASSERT_NE(upper_cap_wrong, vectors.end());
    EXPECT_EQ(Field(upper_cap_wrong->second, "expected_result"), "unexpected_bits");
    EXPECT_EQ(compact(upper_cap_wrong->second, "expected_bits"), testnet_pow_bits);
    EXPECT_NE(compact(upper_cap_wrong->second, "candidate_bits"), testnet_pow_bits);
    const auto upper_cap_wrong_candidate = make_testnet_candidate(upper_cap_wrong->second, false);
    ASSERT_TRUE(upper_cap_wrong_candidate.has_value());
    assert_testnet_record_parent(upper_cap_wrong->second, *upper_cap_wrong_candidate);
    assert_rejection_is_atomic(testnet_chain, testnet_utxos, testnet, accepted_testnet_blocks,
                               *upper_cap_wrong_candidate,
                               "upper retarget saturates at the immutable TESTNET powLimit");

    for (const auto delay : {1200U, 1201U}) {
        for (const auto suffix : {"", "_wrong_bits"}) {
            const auto name = "difficulty_testnet_delay_" + std::to_string(delay) + suffix;
            const auto found = vectors.find(name);
            ASSERT_NE(found, vectors.end()) << name;
            const auto& record = found->second;
            const bool accepted_case = Field(record, "expected_result") == "accept";
            const auto expected_bits = compact(record, "expected_bits");
            const auto candidate_bits = compact(record, "candidate_bits");
            const auto parent_bits = compact(record, "parent_bits");
            const auto accepted_name = "difficulty_testnet_delay_" + std::to_string(delay);
            const auto accepted_record = vectors.find(accepted_name);
            ASSERT_NE(accepted_record, vectors.end());
            ASSERT_TRUE(expected_bits.has_value());
            ASSERT_TRUE(candidate_bits.has_value());
            ASSERT_TRUE(parent_bits.has_value());
            EXPECT_EQ(number(record, "candidate_delay_seconds"), delay);
            EXPECT_EQ(number(record, "parent_time"), testnet.genesis_block.header.time + 302'401U);
            EXPECT_EQ(compact(record, "expected_bits"),
                      compact(accepted_record->second, "expected_bits"));
            EXPECT_EQ(compact(record, "parent_bits"),
                      compact(accepted_record->second, "parent_bits"));
            EXPECT_EQ(*candidate_bits == *expected_bits, accepted_case);
            if (delay == 1200U) {
                EXPECT_EQ(*expected_bits, *parent_bits);
            } else {
                EXPECT_EQ(*expected_bits, testnet_pow_bits);
            }
            auto candidate = make_testnet_candidate(record, accepted_case);
            ASSERT_TRUE(candidate.has_value()) << name;
            assert_testnet_record_parent(record, *candidate);
            if (!accepted_case) {
                assert_rejection_is_atomic(testnet_chain, testnet_utxos, testnet,
                                           accepted_testnet_blocks, *candidate, name);
                continue;
            }
            const auto hash = nova::primitives::ComputeBlockHash(candidate->header);
            ASSERT_TRUE(hash.has_value());
            const auto result = testnet_chain.AcceptBlock(*candidate);
            ASSERT_EQ(result.error, nova::chain::ChainError::kNone) << name;
            EXPECT_EQ(result.block_hash, hash);
            const auto index = testnet_chain.GetBlockIndex(*hash);
            ASSERT_TRUE(index.has_value());
            EXPECT_EQ(index->expected_bits, *expected_bits);
            accepted_testnet_blocks.push_back(*candidate);
        }
    }

    const auto upper_exact = vectors.find("difficulty_testnet_retarget_upper_clamp_exact");
    const auto upper_above = vectors.find("difficulty_testnet_retarget_upper_clamp_above");
    const auto upper_above_wrong =
        vectors.find("difficulty_testnet_retarget_upper_clamp_above_wrong_bits");
    ASSERT_NE(upper_exact, vectors.end());
    ASSERT_NE(upper_above, vectors.end());
    ASSERT_NE(upper_above_wrong, vectors.end());
    const auto upper_base = retarget_children.find(300U);
    ASSERT_NE(upper_base, retarget_children.end());
    const auto upper_base_index = testnet_chain.GetBlockIndex(upper_base->second);
    ASSERT_TRUE(upper_base_index.has_value());
    const auto upper_initial_bits = compact(upper_exact->second, "parent_bits");
    const auto upper_delay_height = number(upper_exact->second, "history_delay_height");
    const auto upper_delay_seconds = number(upper_exact->second, "history_delay_seconds");
    const auto upper_post_delay_blocks = number(upper_exact->second, "history_post_delay_blocks");
    const auto upper_post_delay_spacing =
        number(upper_exact->second, "history_post_delay_spacing_seconds");
    const auto upper_first_height = number(upper_exact->second, "first_interval_height");
    const auto upper_first_time = number(upper_exact->second, "first_interval_time");
    ASSERT_TRUE(upper_initial_bits.has_value());
    ASSERT_TRUE(upper_delay_height.has_value());
    ASSERT_TRUE(upper_delay_seconds.has_value());
    ASSERT_TRUE(upper_post_delay_blocks.has_value());
    ASSERT_TRUE(upper_post_delay_spacing.has_value());
    ASSERT_TRUE(upper_first_height.has_value());
    ASSERT_TRUE(upper_first_time.has_value());
    EXPECT_EQ(*upper_first_height, testnet.difficulty.retarget_interval);
    EXPECT_EQ(upper_base_index->height, *upper_first_height);
    EXPECT_EQ(upper_base_index->time, *upper_first_time);
    EXPECT_EQ(upper_base_index->expected_bits, *upper_initial_bits);
    EXPECT_EQ(*upper_delay_height, *upper_first_height + 1U);
    EXPECT_EQ(*upper_post_delay_blocks, 2'013U);
    EXPECT_EQ(*upper_post_delay_spacing, 1'200U);
    EXPECT_EQ(compact(upper_exact->second, "parent_bits"), *upper_initial_bits);

    auto upper_history_parent_hash = upper_base->second;
    auto upper_history_parent_time = *upper_first_time;
    upper_history_parent_time += *upper_delay_seconds;
    auto delayed_minimum_block =
        MakeDifficultyBlock(upper_history_parent_hash, *upper_delay_height,
                            upper_history_parent_time, testnet_pow_bits, testnet, true);
    ASSERT_TRUE(delayed_minimum_block.has_value());
    auto delayed_minimum_result = testnet_chain.AcceptBlock(*delayed_minimum_block);
    ASSERT_EQ(delayed_minimum_result.error, nova::chain::ChainError::kNone);
    auto delayed_minimum_hash = nova::primitives::ComputeBlockHash(delayed_minimum_block->header);
    ASSERT_TRUE(delayed_minimum_hash.has_value());
    auto delayed_minimum_index = testnet_chain.GetBlockIndex(*delayed_minimum_hash);
    ASSERT_TRUE(delayed_minimum_index.has_value());
    EXPECT_EQ(delayed_minimum_index->expected_bits, testnet_pow_bits);
    accepted_testnet_blocks.push_back(*delayed_minimum_block);
    upper_history_parent_hash = *delayed_minimum_hash;
    auto upper_history_height = *upper_delay_height;

    for (std::uint32_t index = 0U; index < *upper_post_delay_blocks; ++index) {
        ++upper_history_height;
        upper_history_parent_time += *upper_post_delay_spacing;
        auto block =
            MakeDifficultyBlock(upper_history_parent_hash, upper_history_height,
                                upper_history_parent_time, *upper_initial_bits, testnet, true);
        ASSERT_TRUE(block.has_value()) << "upper-clamp history height " << upper_history_height;
        const auto hash = nova::primitives::ComputeBlockHash(block->header);
        ASSERT_TRUE(hash.has_value());
        const auto accepted = testnet_chain.AcceptBlock(*block);
        ASSERT_EQ(accepted.error, nova::chain::ChainError::kNone)
            << "upper-clamp history height " << upper_history_height;
        const auto index_entry = testnet_chain.GetBlockIndex(*hash);
        ASSERT_TRUE(index_entry.has_value());
        EXPECT_EQ(index_entry->expected_bits, *upper_initial_bits);
        accepted_testnet_blocks.push_back(*block);
        upper_history_parent_hash = *hash;
    }
    ASSERT_EQ(upper_history_height, 4'030U);

    const auto upper_fork_parent_hash = upper_history_parent_hash;
    const auto upper_fork_parent_time = upper_history_parent_time;
    std::map<std::string, nova::crypto::Hash256> upper_clamp_parent_hashes;
    for (const auto suffix : {"exact", "above"}) {
        const auto name = std::string{"difficulty_testnet_retarget_upper_clamp_"} + suffix;
        const auto found = vectors.find(name);
        ASSERT_NE(found, vectors.end()) << name;
        const auto& record = found->second;
        const auto final_parent_delta = number(record, "history_final_parent_delta_seconds");
        const auto parent_height = number(record, "parent_height");
        const auto candidate_height = number(record, "candidate_height");
        const auto candidate_time = number(record, "candidate_time");
        const auto candidate_bits = compact(record, "candidate_bits");
        const auto expected_bits = compact(record, "expected_bits");
        ASSERT_TRUE(final_parent_delta.has_value());
        ASSERT_TRUE(parent_height.has_value());
        ASSERT_TRUE(candidate_height.has_value());
        ASSERT_TRUE(candidate_time.has_value());
        ASSERT_TRUE(candidate_bits.has_value());
        ASSERT_TRUE(expected_bits.has_value());
        EXPECT_EQ(*parent_height, 4'031U);
        EXPECT_EQ(*candidate_height, 4'032U);
        EXPECT_EQ(number(record, "candidate_delay_seconds"), 1U);
        EXPECT_EQ(number(record, "expected_elapsed_seconds"),
                  number(record, "parent_time").value() - *upper_first_time);
        EXPECT_EQ(number(record, "clamped_elapsed_seconds"), 4'838'400U);
        EXPECT_EQ(*candidate_bits, *expected_bits);
        EXPECT_EQ(*expected_bits, 0x2070FFFCU);

        const auto upper_parent_time = upper_fork_parent_time + *final_parent_delta;
        EXPECT_EQ(number(record, "parent_time"), upper_parent_time);
        auto parent = MakeDifficultyBlock(upper_fork_parent_hash, *parent_height, upper_parent_time,
                                          *upper_initial_bits, testnet, true);
        ASSERT_TRUE(parent.has_value()) << name;
        const auto parent_hash = nova::primitives::ComputeBlockHash(parent->header);
        ASSERT_TRUE(parent_hash.has_value());
        const auto parent_result = testnet_chain.AcceptBlock(*parent);
        ASSERT_EQ(parent_result.error, nova::chain::ChainError::kNone) << name;
        const auto parent_index = testnet_chain.GetBlockIndex(*parent_hash);
        ASSERT_TRUE(parent_index.has_value());
        EXPECT_EQ(parent_index->expected_bits, *upper_initial_bits);
        accepted_testnet_blocks.push_back(*parent);
        upper_clamp_parent_hashes.emplace(suffix, *parent_hash);

        auto candidate = MakeDifficultyBlock(*parent_hash, *candidate_height, *candidate_time,
                                             *candidate_bits, testnet, true);
        ASSERT_TRUE(candidate.has_value()) << name;
        assert_testnet_record_parent(record, *candidate);
        const auto candidate_hash = nova::primitives::ComputeBlockHash(candidate->header);
        ASSERT_TRUE(candidate_hash.has_value());
        const auto result = testnet_chain.AcceptBlock(*candidate);
        ASSERT_EQ(result.error, nova::chain::ChainError::kNone) << name;
        EXPECT_EQ(result.block_hash, candidate_hash);
        const auto index_entry = testnet_chain.GetBlockIndex(*candidate_hash);
        ASSERT_TRUE(index_entry.has_value());
        EXPECT_EQ(index_entry->expected_bits, *expected_bits);
        accepted_testnet_blocks.push_back(*candidate);
    }

    const auto& upper_wrong_record = upper_above_wrong->second;
    EXPECT_EQ(Field(upper_wrong_record, "expected_result"), "unexpected_bits");
    EXPECT_EQ(compact(upper_wrong_record, "expected_bits"), 0x2070FFFCU);
    EXPECT_EQ(compact(upper_wrong_record, "candidate_bits"), testnet_pow_bits);
    const auto upper_wrong_parent = upper_clamp_parent_hashes.find("above");
    ASSERT_NE(upper_wrong_parent, upper_clamp_parent_hashes.end());
    const auto upper_wrong_height = number(upper_wrong_record, "candidate_height");
    const auto upper_wrong_time = number(upper_wrong_record, "candidate_time");
    const auto upper_wrong_bits = compact(upper_wrong_record, "candidate_bits");
    ASSERT_TRUE(upper_wrong_height.has_value());
    ASSERT_TRUE(upper_wrong_time.has_value());
    ASSERT_TRUE(upper_wrong_bits.has_value());
    auto upper_wrong_candidate =
        MakeDifficultyBlock(upper_wrong_parent->second, *upper_wrong_height, *upper_wrong_time,
                            *upper_wrong_bits, testnet, false);
    ASSERT_TRUE(upper_wrong_candidate.has_value());
    assert_testnet_record_parent(upper_wrong_record, *upper_wrong_candidate);
    assert_rejection_is_atomic(testnet_chain, testnet_utxos, testnet, accepted_testnet_blocks,
                               *upper_wrong_candidate,
                               "upper-clamped bits reject the pow-limit candidate");

    const auto& regtest = nova::consensus::RegtestNetworkParams();
    ASSERT_TRUE(regtest.difficulty.no_retargeting);
    const auto regtest_now = std::make_shared<nova::chain::FixedValidationTimeSource>(
        regtest.genesis_block.header.time + 10'000U);
    auto regtest_initialized = nova::chain::NetworkChainInitializer::Create(
        nova::consensus::NetworkId::kRegtest, regtest_now);
    ASSERT_TRUE(regtest_initialized.has_value());
    auto& regtest_chain = *regtest_initialized->chain;
    auto& regtest_utxos = *regtest_initialized->utxos;
    const auto regtest_valid = vectors.find("difficulty_regtest_no_retarget");
    const auto regtest_wrong = vectors.find("difficulty_regtest_no_retarget_wrong_bits");
    ASSERT_NE(regtest_valid, vectors.end());
    ASSERT_NE(regtest_wrong, vectors.end());
    EXPECT_EQ(ParseCanonicalUnsigned<std::uint32_t>(Field(regtest_valid->second, "genesis_time")),
              regtest.genesis_block.header.time);
    EXPECT_EQ(ParseCanonicalCompact(Field(regtest_valid->second, "pow_limit_bits")),
              regtest.pow.pow_limit_compact);
    EXPECT_EQ(number(regtest_valid->second, "retarget_interval"),
              regtest.difficulty.retarget_interval);
    EXPECT_EQ(number(regtest_valid->second, "target_spacing_seconds"),
              regtest.difficulty.target_spacing_seconds);
    EXPECT_EQ(number(regtest_valid->second, "target_timespan_seconds"),
              regtest.difficulty.target_timespan_seconds);
    EXPECT_EQ(Field(regtest_valid->second, "expected_result"), "accept");
    const auto regtest_height = number(regtest_valid->second, "candidate_height");
    const auto regtest_time = number(regtest_valid->second, "candidate_time");
    const auto regtest_bits = compact(regtest_valid->second, "candidate_bits");
    ASSERT_TRUE(regtest_height.has_value());
    ASSERT_TRUE(regtest_time.has_value());
    ASSERT_TRUE(regtest_bits.has_value());
    EXPECT_EQ(number(regtest_valid->second, "parent_height"), 0U);
    EXPECT_EQ(number(regtest_valid->second, "candidate_height"), 1U);
    EXPECT_EQ(number(regtest_valid->second, "parent_time"), regtest.genesis_block.header.time);
    EXPECT_EQ(number(regtest_valid->second, "candidate_delay_seconds"),
              *regtest_time - regtest.genesis_block.header.time);
    EXPECT_EQ(compact(regtest_valid->second, "parent_bits"), regtest.genesis_block.header.bits);
    EXPECT_EQ(compact(regtest_valid->second, "expected_bits"), *regtest_bits);
    auto regtest_block = MakeDifficultyBlock(regtest.genesis_hash, *regtest_height, *regtest_time,
                                             *regtest_bits, regtest, true);
    ASSERT_TRUE(regtest_block.has_value());
    EXPECT_EQ(regtest_block->header.previous_block_id, regtest.genesis_hash);
    const auto regtest_hash = nova::primitives::ComputeBlockHash(regtest_block->header);
    ASSERT_TRUE(regtest_hash.has_value());
    EXPECT_EQ(regtest_chain.AcceptBlock(*regtest_block).error, nova::chain::ChainError::kNone);
    const auto regtest_index = regtest_chain.GetBlockIndex(*regtest_hash);
    ASSERT_TRUE(regtest_index.has_value());
    EXPECT_EQ(regtest_index->expected_bits, *regtest_bits);
    std::vector<nova::primitives::Block> accepted_regtest_blocks{regtest.genesis_block,
                                                                 *regtest_block};
    const auto wrong_bits = compact(regtest_wrong->second, "candidate_bits");
    ASSERT_TRUE(wrong_bits.has_value());
    EXPECT_EQ(Field(regtest_wrong->second, "expected_result"), "unexpected_bits");
    EXPECT_EQ(compact(regtest_wrong->second, "expected_bits"),
              compact(regtest_valid->second, "expected_bits"));
    EXPECT_EQ(compact(regtest_wrong->second, "parent_bits"),
              compact(regtest_valid->second, "parent_bits"));
    EXPECT_NE(wrong_bits, compact(regtest_wrong->second, "expected_bits"));
    auto regtest_wrong_block = MakeDifficultyBlock(
        regtest.genesis_hash, number(regtest_wrong->second, "candidate_height").value(),
        number(regtest_wrong->second, "candidate_time").value(), *wrong_bits, regtest, false);
    ASSERT_TRUE(regtest_wrong_block.has_value());
    EXPECT_EQ(regtest_wrong_block->header.previous_block_id, regtest.genesis_hash);
    assert_rejection_is_atomic(regtest_chain, regtest_utxos, regtest, accepted_regtest_blocks,
                               *regtest_wrong_block,
                               "REGTEST no-retarget inheritance rejects altered bits");
}

TEST(ProtocolConformance, DifficultyVectorSchemaFailsClosed)
{
    const auto valid = ReadRecord("difficulty_testnet_retarget_302400.ncpv");
    ASSERT_TRUE(valid.has_value());
    ASSERT_TRUE(DifficultyVectorSchema(*valid));
    auto malformed = *valid;
    malformed["unknown"] = "1";
    EXPECT_FALSE(DifficultyVectorSchema(malformed));
    for (const auto& [name, value] : *valid) {
        static_cast<void>(value);
        malformed = *valid;
        malformed.erase(name);
        EXPECT_FALSE(DifficultyVectorSchema(malformed)) << name;
    }
    for (const auto& [name, value] :
         {std::pair{"expected_result", "unknown"}, std::pair{"network", "MAINNET"},
          std::pair{"candidate_bits", "201C3fff"}, std::pair{"candidate_delay_seconds", "01200"},
          std::pair{"scale_remainder", "-1"}, std::pair{"kind", "difficulty_adjustment_extra"}}) {
        malformed = *valid;
        malformed[name] = value;
        EXPECT_FALSE(DifficultyVectorSchema(malformed)) << name;
    }
    malformed = *valid;
    malformed["expected_result"] = "unexpected_bits";
    malformed["candidate_bits"] = "201c3ffe";
    EXPECT_TRUE(DifficultyVectorSchema(malformed));
}

TEST(ProtocolConformance, PureVectorSchemaAndExpectedResultsFailClosed)
{
    for (const auto filename :
         {"compact_context_valid.ncpv", "work_context_valid.ncpv", "pow_context_equal.ncpv"}) {
        const auto fixture = ReadRecord(filename);
        ASSERT_TRUE(fixture.has_value());
        ASSERT_TRUE(ReplayPureVector(*fixture));
        auto unknown = *fixture;
        unknown["expected_result"] = "unknown";
        EXPECT_FALSE(ReplayPureVector(unknown));
        unknown = *fixture;
        unknown["unexpected"] = "1";
        EXPECT_FALSE(ReplayPureVector(unknown));
        unknown = *fixture;
        unknown["target"] = "00";
        EXPECT_FALSE(ReplayPureVector(unknown));
        for (const auto& [name, value] : *fixture) {
            static_cast<void>(value);
            auto missing = *fixture;
            missing.erase(name);
            EXPECT_FALSE(ReplayPureVector(missing)) << filename << ':' << name;
        }
    }
    const auto record = ReadRecord("sighash_context_valid.ncpv");
    ASSERT_TRUE(record.has_value());
    ASSERT_TRUE(ReplayPureVector(*record));
    for (const auto& [name, value] :
         {std::pair{"unknown", "1"}, std::pair{"kind", "unknown"},
          std::pair{"expected_result", "unknown"}, std::pair{"expected_digest", "00"},
          std::pair{"input_index", "18446744073709551616"}, std::pair{"input_index", "01"},
          std::pair{"network", "MAINNET"}, std::pair{"sighash_selector", "4294967296"}}) {
        auto malformed = *record;
        malformed[name] = value;
        EXPECT_FALSE(ReplayPureVector(malformed)) << name << '=' << value;
    }
    for (const auto& [name, value] : *record) {
        static_cast<void>(value);
        auto missing = *record;
        missing.erase(name);
        EXPECT_FALSE(ReplayPureVector(missing)) << name;
    }
    auto wrong_preimage = *record;
    wrong_preimage["expected_preimage"].back() = '2';
    EXPECT_FALSE(ReplayPureVector(wrong_preimage));
    auto maximum_index = *record;
    maximum_index["input_index"] = "18446744073709551615";
    maximum_index["expected_result"] = "unavailable";
    maximum_index["expected_digest"] = "none";
    maximum_index["expected_preimage"] = "none";
    if constexpr (sizeof(std::size_t) == sizeof(std::uint64_t))
        EXPECT_TRUE(ReplayPureVector(maximum_index));
    else
        EXPECT_FALSE(ReplayPureVector(maximum_index));
}

TEST(ProtocolConformance, OwnerOutpointPredicatesAndExistenceAreSeparate)
{
    nova::crypto::Hash256::Bytes nonzero{};
    nonzero.back() = 1U;
    for (const auto index : {0U, 0xfffffffeU, 0xffffffffU}) {
        const nova::primitives::OutPoint null_hash{nova::crypto::Hash256{}, index};
        EXPECT_EQ(null_hash.IsNull(), index == 0xffffffffU);
        EXPECT_EQ(null_hash.IsMalformed(), index != 0xffffffffU);
        const nova::primitives::OutPoint nonnull{nova::crypto::Hash256{nonzero}, index};
        EXPECT_FALSE(nonnull.IsNull());
        EXPECT_FALSE(nonnull.IsMalformed());
    }
    auto fixture = ParseSignatureAuthorizationRecord(SignatureAuthorizationFixture());
    ASSERT_TRUE(fixture.has_value());
    const auto parameters =
        nova::chain::NetworkValidationParams(nova::consensus::RegtestNetworkParams());
    nova::chain::UTXOSet empty;
    fixture->transaction.inputs.front().previous_output.output_index = 0xffffffffU;
    EXPECT_EQ(nova::chain::ValidateTransactionAgainstUTXOSet(fixture->transaction, {1U, 1U},
                                                             parameters, empty)
                  .error,
              nova::chain::BlockValidationError::kMissingUtxo);
    EXPECT_EQ(empty.size(), 0U);
}

TEST(ProtocolConformance, OwnerBoundedParsingRejectsTrailingAndTruncatedTransactions)
{
    const auto fixture = SignatureAuthorizationFixture();
    auto bytes = HexBytes(Field(fixture, "transaction"));
    ASSERT_TRUE(bytes.has_value());
    const auto& limits = nova::consensus::RegtestNetworkParams().block_limits.transaction_limits;
    ASSERT_TRUE(nova::primitives::Transaction::Deserialize(*bytes, limits).has_value());
    bytes->push_back(0U);
    EXPECT_FALSE(nova::primitives::Transaction::Deserialize(*bytes, limits).has_value());
    bytes->pop_back();
    bytes->pop_back();
    EXPECT_FALSE(nova::primitives::Transaction::Deserialize(*bytes, limits).has_value());
    // This asserts admission, not allocator instrumentation or allocation timing.
}

TEST(ProtocolConformance, OwnerDerScalarAndLowSBoundaries)
{
    // R/S below are public signature components, never private key material.
    for (const auto& [hex, accepted] : std::array<std::pair<std::string_view, bool>, 9>{
             {{"3006020101020101", true},
              {"3006020100020101", false},
              {"3006020101020100", false},
              {"3006020180020101", false},
              {"300702020001020101", false},
              {"300602010102010100", false},
              {"302502010102207fffffffffffffffffffffffffffffff5d576e7357a4501ddfe92f46681b20a0",
               true},
              {"302502010102207fffffffffffffffffffffffffffffff5d576e7357a4501ddfe92f46681b20a1",
               false},
              {"3026022100fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141020101",
               false}}}) {
        SCOPED_TRACE(hex);
        const auto bytes = HexBytes(hex);
        ASSERT_TRUE(bytes.has_value());
        const auto signature = nova::crypto::Signature::FromDer(*bytes);
        EXPECT_EQ(signature.has_value(), accepted);
        if (signature.has_value()) {
            EXPECT_EQ(HexText(signature->SerializeDer()), hex);
        }
    }
}

TEST(ProtocolConformance, OwnerCompressedKeyBoundaries)
{
    const auto fixture = ParseSignatureAuthorizationRecord(SignatureAuthorizationFixture());
    ASSERT_TRUE(fixture.has_value());
    const auto& script = fixture->transaction.inputs.front().script_sig;
    ASSERT_GE(script.size(), 33U);
    std::vector<std::uint8_t> key(script.end() - 33, script.end());
    ASSERT_TRUE(nova::crypto::PublicKey::FromCompressed(key).has_value());
    auto invalid_prefix = key;
    invalid_prefix.front() = 4U;
    EXPECT_FALSE(nova::crypto::PublicKey::FromCompressed(invalid_prefix).has_value());
    key.pop_back();
    EXPECT_FALSE(nova::crypto::PublicKey::FromCompressed(key).has_value());
    key.resize(34U, 0U);
    EXPECT_FALSE(nova::crypto::PublicKey::FromCompressed(key).has_value());
    std::vector<std::uint8_t> invalid_point(33U, 0xffU);
    invalid_point.front() = 2U;
    EXPECT_FALSE(nova::crypto::PublicKey::FromCompressed(invalid_point).has_value());
}

TEST(ProtocolConformance, OwnerExactFullSchedulesAndFinalEra)
{
    for (const auto& [network, expected] :
         std::array<std::pair<const nova::consensus::NetworkParams*, std::int64_t>, 2>{
             {{&nova::consensus::RegtestNetworkParams(), 1499999998350LL},
              {&nova::consensus::TestnetNetworkParams(), 2099999997690000LL}}}) {
        SCOPED_TRACE(network->monetary.halving_interval);
        EXPECT_EQ(nova::consensus::CalculateTheoreticalIssuance(network->monetary), expected);
        std::int64_t independent_sum = 0;
        auto subsidy = network->monetary.initial_subsidy;
        while (subsidy != 0) {
            independent_sum +=
                subsidy * static_cast<std::int64_t>(network->monetary.halving_interval);
            subsidy /= 2;
        }
        EXPECT_EQ(independent_sum, expected);
        const auto interval = network->monetary.halving_interval;
        EXPECT_EQ(nova::consensus::GetBlockSubsidy(32U * interval, network->monetary), 1);
        EXPECT_EQ(nova::consensus::GetBlockSubsidy(33U * interval - 1U, network->monetary), 1);
        EXPECT_EQ(nova::consensus::GetBlockSubsidy(33U * interval, network->monetary), 0);
        auto invalid = network->monetary;
        invalid.halving_interval = 0U;
        EXPECT_FALSE(nova::consensus::CalculateTheoreticalIssuance(invalid).has_value());
    }
    EXPECT_LE(2099999997690000LL + nova::consensus::TESTNET_CREATOR_ALLOCATION,
              nova::consensus::MAX_MONEY);
}

TEST(ProtocolConformance, OwnerCompactEncodingTruncatesWithoutRoundingUp)
{
    for (const auto& [magnitude, compact] :
         std::array<std::pair<std::string_view, std::uint32_t>, 5>{
             {{"0000000000000000000000000000000000000000000000000000000000000001", 0x01010000U},
              {"0000000000000000000000000000000000000000000000000000000000000080", 0x02008000U},
              {"0000000000000000000000000000000000000000000000000000000000800000", 0x04008000U},
              {"0000000000000000000000000000000000000000000000000000000001234567", 0x04012345U},
              {"00000000000000000000000000000000000000000000000000000000012345ff", 0x04012345U}}}) {
        const auto target = nova::consensus::Target256::FromHex(magnitude);
        ASSERT_TRUE(target.has_value());
        EXPECT_EQ(nova::consensus::CompactFromTarget(*target), compact);
        const auto decoded = nova::consensus::TargetFromCompact(compact);
        ASSERT_TRUE(decoded.target.has_value());
        EXPECT_FALSE(*target < *decoded.target);
        EXPECT_EQ(nova::consensus::CompactFromTarget(*decoded.target), compact);
    }
    EXPECT_EQ(nova::consensus::TargetFromCompact(0x01010001U).error,
              nova::consensus::TargetError::kNonCanonical);
    EXPECT_EQ(nova::consensus::TargetFromCompact(0x00800001U).error,
              nova::consensus::TargetError::kNegative);
}

} // namespace
