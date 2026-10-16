#include "chain/validation.hpp"
#include "consensus/network_params.hpp"
#include "net/transport.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using nova::primitives::Transaction;
constexpr std::size_t kMaximumFixtureBytes = 4096U;

std::string Hex(const std::span<const std::uint8_t> bytes)
{
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result;
    for (const auto byte : bytes) {
        result += digits[byte >> 4U];
        result += digits[byte & 15U];
    }
    return result;
}

std::optional<Transaction> Decode(const std::string_view text)
{
    if (text.empty() || text.size() > kMaximumFixtureBytes * 2U || text.size() % 2U != 0U) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0; index < text.size(); index += 2U) {
        unsigned int value{};
        const auto parsed =
            std::from_chars(text.data() + index, text.data() + index + 2U, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + index + 2U) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    const auto& limits = nova::consensus::RegtestNetworkParams().block_limits.transaction_limits;
    auto transaction = Transaction::Deserialize(bytes, limits);
    nova::primitives::BinaryWriter writer;
    if (!transaction || !transaction->Serialize(writer, limits) ||
        !std::ranges::equal(writer.bytes(), bytes)) {
        return std::nullopt;
    }
    return transaction;
}

std::optional<Transaction> Read(const char* path)
{
    std::ifstream file{path, std::ios::binary};
    std::string text;
    char character{};
    while (file.get(character)) {
        if (text.size() >= kMaximumFixtureBytes * 2U + 1U) {
            return std::nullopt;
        }
        text += character;
    }
    if (!file.eof()) {
        return std::nullopt;
    }
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    return Decode(text);
}

nova::chain::BlockValidationParams Parameters()
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    return {network.block_limits,
            network.pow,
            network.monetary,
            0U,
            500'000'000U,
            std::numeric_limits<std::uint32_t>::max(),
            1U,
            600U};
}

// Narrow fixture: one non-coinbase output from the supplied parent block.
// Never infer its amount or locking script from the transaction being signed.
std::optional<nova::primitives::TxOutput> PreviousOutput(const Transaction& transaction,
                                                         const nova::primitives::Block& parent)
{
    if (transaction.inputs.size() != 1U || transaction.outputs.empty() ||
        transaction.lock_time != 0U) {
        return std::nullopt;
    }
    const auto& outpoint = transaction.inputs.front().previous_output;
    const auto& limits = nova::consensus::RegtestNetworkParams().block_limits.transaction_limits;
    for (std::size_t index = 1U; index < parent.transactions.size(); ++index) {
        const auto& previous = parent.transactions[index];
        if (previous.TxId(limits) == outpoint.transaction_id &&
            outpoint.output_index < previous.outputs.size()) {
            const auto& output = previous.outputs[outpoint.output_index];
            if (output.value > 0 && output.value < limits.max_money &&
                output.script_pubkey.size() == 25U) {
                return output;
            }
        }
    }
    return std::nullopt;
}

bool Preflight(const Transaction& transaction, const nova::primitives::TxOutput& previous)
{
    const auto parameters = Parameters();
    const auto& limits = parameters.block_limits.transaction_limits;
    if (transaction.inputs.size() != 1U || transaction.outputs.size() != 1U ||
        transaction.lock_time != 0U || previous.value <= 0 || previous.value >= limits.max_money ||
        transaction.outputs.front().value != previous.value + 1) {
        return false;
    }
    nova::primitives::BinaryWriter writer;
    if (!transaction.Serialize(writer, limits) || !Decode(Hex(writer.bytes()))) {
        return false;
    }
    const auto key = nova::chain::UTXOKey::FromOutPoint(transaction.inputs.front().previous_output);
    nova::chain::UTXOSet actual;
    if (!actual.AddCoin(key, {previous, 3U, false}) ||
        nova::chain::ValidateTransactionAgainstUTXOSet(transaction, {4U, 0U}, parameters, actual)
                .error != nova::chain::BlockValidationError::kInputValueTooLow) {
        return false;
    }
    // An independent value-balanced view must fully validate the SAME signed
    // bytes. This proves strict script/signature authorization succeeds, not
    // merely that a signing RPC returned hex. No node UTXO is modified.
    auto balanced_output = previous;
    balanced_output.value += 1;
    nova::chain::UTXOSet balanced;
    return balanced.AddCoin(key, {balanced_output, 3U, false}) &&
           nova::chain::ValidateTransactionAgainstUTXOSet(transaction, {4U, 0U}, parameters,
                                                          balanced)
                   .error == nova::chain::BlockValidationError::kNone;
}

bool SelfTest()
{
    nova::crypto::PrivateKey::Bytes bytes{};
    bytes.back() = 1U; // Public deterministic test key, never a value-bearing key.
    const auto key = nova::crypto::PrivateKey::FromBytes(bytes);
    const auto public_key = key ? key->DerivePublicKey() : std::nullopt;
    const auto hash =
        public_key ? nova::crypto::Hash160Digest(public_key->SerializeCompressed()) : std::nullopt;
    if (!hash) {
        return false;
    }
    std::vector<std::uint8_t> script{0x76U, 0xa9U, 0x14U};
    script.insert(script.end(), hash->begin(), hash->end());
    script.insert(script.end(), {0x88U, 0xacU});
    const nova::primitives::TxOutput previous{100000000, script};
    auto parent = nova::consensus::RegtestNetworkParams().genesis_block;
    const auto limits = Parameters().block_limits.transaction_limits;
    const auto anchor = parent.transactions.front().TxId(limits);
    if (!anchor) {
        return false;
    }
    parent.transactions.push_back(Transaction{1, {{{*anchor, 0U}, {}, 0U}}, {previous}, 0U});
    const auto id = parent.transactions.back().TxId(limits);
    if (!id) {
        return false;
    }
    Transaction transaction{1, {{{*id, 0U}, {}, 0xffffffffU}}, {{100000001, script}}, 0U};
    const auto digest = nova::chain::ComputeSignatureHash(transaction, 0U, script, limits, 1U);
    const auto signature = digest ? key->Sign(*digest) : std::nullopt;
    if (!signature) {
        return false;
    }
    const auto der = signature->SerializeDer();
    auto& unlocking = transaction.inputs.front().script_sig;
    unlocking.push_back(static_cast<std::uint8_t>(der.size() + 1U));
    unlocking.insert(unlocking.end(), der.begin(), der.end());
    unlocking.insert(unlocking.end(), {1U, 0x21U});
    const auto compressed = public_key->SerializeCompressed();
    unlocking.insert(unlocking.end(), compressed.begin(), compressed.end());
    if (!PreviousOutput(transaction, parent) || !Preflight(transaction, previous) || Decode("00") ||
        Decode("0g") || Decode(std::string(kMaximumFixtureBytes * 2U + 2U, '0'))) {
        return false;
    }
    nova::primitives::BinaryWriter writer;
    if (!transaction.Serialize(writer, limits) || Decode(Hex(writer.bytes()) + "00")) {
        return false;
    }
    auto bad = transaction;
    bad.inputs.front().script_sig.clear();
    if (Preflight(bad, previous)) {
        return false;
    }
    bad = transaction;
    bad.outputs.front().value -= 1;
    if (Preflight(bad, previous)) {
        return false;
    }
    bad.inputs.front().previous_output.output_index = 1U;
    return !PreviousOutput(bad, parent) &&
           !PreviousOutput(transaction, nova::consensus::RegtestNetworkParams().genesis_block);
}

std::uint64_t Now()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}
} // namespace

int main(const int argc, const char* const* argv)
{
    try {
        if (argc == 2 && std::string_view{argv[1]} == "--self-test") {
            if (!SelfTest()) {
                return 1;
            }
            std::cout << "signed_overspend_self_test=passed\n";
            return 0;
        }
        if (argc != 5 ||
            (std::string_view{argv[1]} != "prepare" && std::string_view{argv[1]} != "submit" &&
             std::string_view{argv[1]} != "control")) {
            return 2;
        }
        const bool prepare = std::string_view{argv[1]} == "prepare";
        const bool control = std::string_view{argv[1]} == "control";
        unsigned int port{};
        const std::string_view port_text{argv[2]};
        const auto parsed =
            std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        const auto expected = nova::crypto::Hash256::FromHex(argv[3]);
        auto transaction = Read(argv[4]);
        if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() ||
            port == 0U || port > 65535U || !expected || !transaction) {
            return 2;
        }
        const auto& network = nova::consensus::RegtestNetworkParams();
        const nova::net::P2PParams protocol{network.network_magic,
                                            network.minimum_peer_protocol_version,
                                            network.block_limits.max_serialized_size,
                                            256U,
                                            1000U,
                                            50000U,
                                            101U,
                                            2000U,
                                            network.block_limits.transaction_limits,
                                            network.block_limits};
        const auto now = Now();
        const nova::net::ConnectionParams connection{protocol,
                                                     {network.protocol_version, 0U,
                                                      static_cast<std::int64_t>(now), 43U,
                                                      "/NovaCoin-invalid-tx-test/", 0, false},
                                                     5U,
                                                     10U,
                                                     10U,
                                                     100U,
                                                     16U,
                                                     4U * 1024U * 1024U};
        auto transport = nova::net::TcpTransport::Create(
            {{connection, 0U, 1U}, 64U * 1024U, 4U * 1024U * 1024U, 1U});
        if (!transport || !transport->Dial({"127.0.0.1", static_cast<std::uint16_t>(port)}, now)) {
            return 1;
        }
        bool requested{};
        bool submitted{};
        bool failed{};
        bool prepared{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        for (unsigned int pumps = 0; pumps < 2000U && std::chrono::steady_clock::now() < deadline;
             ++pumps) {
            const bool previously_submitted = submitted;
            const auto result = transport->Pump(
                Now(), [&](const std::uint64_t id, const nova::net::FramedMessage& message) {
                    if (message.command == nova::net::Command::kVerack && !requested) {
                        requested = true;
                        const nova::net::InventoryMessage request{{{2U, *expected}}};
                        failed = transport->Queue(id, {nova::net::Command::kGetData, request}) !=
                                 nova::net::TransportError::kNone;
                    } else if (message.command == nova::net::Command::kBlock && !submitted &&
                               !prepared) {
                        const auto* block = std::get_if<nova::primitives::Block>(&message.message);
                        if (!block ||
                            nova::primitives::ComputeBlockHash(block->header) != expected ||
                            nova::primitives::ComputeMerkleRoot(
                                block->transactions, network.block_limits.transaction_limits) !=
                                block->header.merkle_root ||
                            nova::primitives::CheckBlockStructure(*block, network.block_limits) !=
                                nova::primitives::BlockStructureError::kNone) {
                            failed = true;
                            return;
                        }
                        const auto previous = PreviousOutput(*transaction, *block);
                        if (!previous) {
                            failed = true;
                            return;
                        }
                        const auto& outpoint = transaction->inputs.front().previous_output;
                        std::cerr << "parent_hash=" << Hex(expected->bytes()) << '\n'
                                  << "input_txid=" << Hex(outpoint.transaction_id.bytes()) << '\n'
                                  << "input_vout=" << outpoint.output_index << '\n'
                                  << "input_value=" << previous->value << '\n'
                                  << "input_script=" << Hex(previous->script_pubkey) << '\n';
                        nova::primitives::BinaryWriter parent_writer;
                        if (!block->Serialize(parent_writer, network.block_limits)) {
                            failed = true;
                            return;
                        }
                        std::cerr << "parent_hex=" << Hex(parent_writer.bytes()) << '\n';
                        if (prepare) {
                            transaction->outputs.resize(1U);
                            transaction->outputs.front().value = previous->value + 1;
                            transaction->inputs.front().script_sig.clear();
                            nova::primitives::BinaryWriter writer;
                            failed = !transaction->Serialize(
                                writer, network.block_limits.transaction_limits);
                            if (!failed) {
                                std::cout << Hex(writer.bytes()) << '\n';
                                prepared = true;
                            }
                        } else {
                            nova::chain::UTXOSet actual;
                            const bool valid_control =
                                control &&
                                actual.AddCoin(nova::chain::UTXOKey::FromOutPoint(outpoint),
                                               {*previous, 3U, false}) &&
                                nova::chain::ValidateTransactionAgainstUTXOSet(
                                    *transaction, {4U, 0U}, Parameters(), actual)
                                        .error == nova::chain::BlockValidationError::kNone;
                            if ((!control && !Preflight(*transaction, *previous)) ||
                                (control && !valid_control)) {
                                failed = true;
                                return;
                            }
                            std::cerr
                                << "canonical_roundtrip=passed\n"
                                   "signature_authorization=passed\n"
                                << (control ? "preflight=kNone\n" : "preflight=kInputValueTooLow\n")
                                << "transaction_id="
                                << Hex(transaction->TxId(network.block_limits.transaction_limits)
                                           ->bytes())
                                << '\n';
                            failed =
                                transport->Queue(id, {nova::net::Command::kTx, *transaction}) !=
                                nova::net::TransportError::kNone;
                            submitted = !failed;
                        }
                    }
                });
            if (failed || result.error != nova::net::TransportError::kNone) {
                return 1;
            }
            if (prepared) {
                return 0;
            }
            if (control && previously_submitted) {
                std::cerr << "valid_control_flushed=true\n";
                return 0;
            }
            if (transport->peer_count() == 0U) {
                if (requested && submitted) {
                    std::cerr << "peer_closed_after_submission=true\n";
                    return 0;
                }
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        std::cerr << "bounded transaction peer timed out\n";
        return 1;
    } catch (...) {
        std::cerr << "transaction peer failed\n";
        return 1;
    }
}
