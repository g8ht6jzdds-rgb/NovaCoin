#include "explorer/http.hpp"

#include "wallet/address.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace nova::explorer
{
namespace
{

[[nodiscard]] bool IsLoopback(const std::string_view address) noexcept
{
    return address == "127.0.0.1" || address == "::1";
}

[[nodiscard]] bool IsValidCredential(const std::string_view credential,
                                     const bool is_username) noexcept
{
    if (credential.empty()) {
        return false;
    }
    for (const auto character : credential) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x21U || byte == 0x7FU || (is_username && character == ':')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ConstantTimeEqual(const std::string_view left,
                                     const std::string_view right) noexcept
{
    const auto maximum = std::max(left.size(), right.size());
    std::uint8_t difference = static_cast<std::uint8_t>(left.size() != right.size());
    for (std::size_t index = 0U; index < maximum; ++index) {
        const auto left_byte = index < left.size() ? static_cast<std::uint8_t>(left[index]) : 0U;
        const auto right_byte = index < right.size() ? static_cast<std::uint8_t>(right[index]) : 0U;
        difference |= static_cast<std::uint8_t>(left_byte ^ right_byte);
    }
    return difference == 0U;
}

[[nodiscard]] std::optional<std::string> DecodeBasic(const std::string_view authorization) noexcept
{
    constexpr std::string_view kPrefix{"Basic "};
    if (!authorization.starts_with(kPrefix)) {
        return std::nullopt;
    }
    const auto encoded = authorization.substr(kPrefix.size());
    if (encoded.empty() || (encoded.size() % 4U) != 0U) {
        return std::nullopt;
    }
    try {
        const auto value = [](const char character) -> std::optional<std::uint8_t> {
            if (character >= 'A' && character <= 'Z') {
                return static_cast<std::uint8_t>(character - 'A');
            }
            if (character >= 'a' && character <= 'z') {
                return static_cast<std::uint8_t>(character - 'a' + 26);
            }
            if (character >= '0' && character <= '9') {
                return static_cast<std::uint8_t>(character - '0' + 52);
            }
            if (character == '+') {
                return 62U;
            }
            if (character == '/') {
                return 63U;
            }
            return std::nullopt;
        };
        std::string decoded;
        decoded.reserve((encoded.size() / 4U) * 3U);
        for (std::size_t index = 0U; index < encoded.size(); index += 4U) {
            const bool third_padding = encoded[index + 2U] == '=';
            const bool fourth_padding = encoded[index + 3U] == '=';
            if (third_padding && !fourth_padding) {
                return std::nullopt;
            }
            const auto first = value(encoded[index]);
            const auto second = value(encoded[index + 1U]);
            const auto third =
                third_padding ? std::optional<std::uint8_t>{0U} : value(encoded[index + 2U]);
            const auto fourth =
                fourth_padding ? std::optional<std::uint8_t>{0U} : value(encoded[index + 3U]);
            if (!first.has_value() || !second.has_value() || !third.has_value() ||
                !fourth.has_value() ||
                ((third_padding || fourth_padding) && index + 4U != encoded.size())) {
                return std::nullopt;
            }
            if ((third_padding && (*second & 0x0FU) != 0U) ||
                (fourth_padding && !third_padding && (*third & 0x03U) != 0U)) {
                return std::nullopt;
            }
            decoded.push_back(static_cast<char>((*first << 2U) | (*second >> 4U)));
            if (!third_padding) {
                decoded.push_back(static_cast<char>((*second << 4U) | (*third >> 2U)));
            }
            if (!fourth_padding) {
                decoded.push_back(static_cast<char>((*third << 6U) | *fourth));
            }
        }
        return decoded;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::string Hex(const std::span<const std::uint8_t> bytes)
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

[[nodiscard]] std::string HashHex(const crypto::Hash256& hash)
{
    return Hex(hash.bytes());
}

[[nodiscard]] std::string AddressText(const crypto::Hash160& address,
                                      const consensus::NetworkParams& network)
{
    const auto encoded = wallet::EncodeP2pkhAddress(address, network);
    return encoded.value_or("");
}

[[nodiscard]] const char* NetworkName(const consensus::NetworkId network) noexcept
{
    switch (network) {
    case consensus::NetworkId::kRegtest:
        return "regtest";
    case consensus::NetworkId::kTestnet:
        return "testnet";
    case consensus::NetworkId::kMainnet:
        return "mainnet";
    }
    return "invalid";
}

[[nodiscard]] std::optional<std::size_t> ParseLimit(const std::string_view query,
                                                    const std::size_t maximum) noexcept
{
    constexpr std::string_view kPrefix{"?limit="};
    if (!query.starts_with(kPrefix) || query.size() == kPrefix.size()) {
        return std::nullopt;
    }
    std::size_t value{};
    const auto text = query.substr(kPrefix.size());
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0U ||
        value > maximum) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] ExplorerHttpResponse Error(const std::uint16_t status, const std::string_view message)
{
    return {status, "application/json", "{\"error\":\"" + std::string{message} + "\"}"};
}

[[nodiscard]] std::string UtxoJson(const ExplorerUtxo& utxo,
                                   const consensus::NetworkParams& network)
{
    std::string result{"{\"txid\":\"" + HashHex(utxo.key.transaction_id) +
                       "\",\"vout\":" + std::to_string(utxo.key.output_index) +
                       ",\"value\":" + std::to_string(utxo.coin.output.value) +
                       ",\"height\":" + std::to_string(utxo.coin.height) + ",\"coinbase\":" +
                       (utxo.coin.is_coinbase ? "true" : "false") + ",\"script_pubkey\":\"" +
                       Hex(utxo.coin.output.script_pubkey) + "\",\"address\":"};
    if (utxo.address.has_value()) {
        result += "\"" + AddressText(*utxo.address, network) + "\"";
    } else {
        result += "null";
    }
    return result + "}";
}

[[nodiscard]] std::string TransactionJson(const ExplorerTransaction& transaction,
                                          const consensus::NetworkParams& network)
{
    std::string result{"{\"txid\":\"" + HashHex(transaction.transaction_id) +
                       "\",\"block_hash\":\"" + HashHex(transaction.block_hash) +
                       "\",\"height\":" + std::to_string(transaction.block_height) +
                       ",\"coinbase\":" + (transaction.is_coinbase ? "true" : "false") +
                       ",\"fee\":"};
    result += transaction.fee.has_value() ? std::to_string(*transaction.fee) : "null";
    result += ",\"inputs\":[";
    for (std::size_t index = 0U; index < transaction.transaction.inputs.size(); ++index) {
        const auto& input = transaction.transaction.inputs.at(index);
        if (index != 0U) {
            result += ',';
        }
        result += "{\"txid\":\"" + HashHex(input.previous_output.transaction_id) +
                  "\",\"vout\":" + std::to_string(input.previous_output.output_index) +
                  ",\"script_sig\":\"" + Hex(input.script_sig) +
                  "\",\"sequence\":" + std::to_string(input.sequence) + "}";
    }
    result += "],\"outputs\":[";
    for (std::size_t index = 0U; index < transaction.transaction.outputs.size(); ++index) {
        const auto& output = transaction.transaction.outputs.at(index);
        if (index != 0U) {
            result += ',';
        }
        result += "{\"vout\":" + std::to_string(index) +
                  ",\"value\":" + std::to_string(output.value) + ",\"script_pubkey\":\"" +
                  Hex(output.script_pubkey) + "\",\"address\":";
        const auto address = ExtractP2pkhAddress(output.script_pubkey);
        result += address.has_value() ? "\"" + AddressText(*address, network) + "\"" : "null";
        result += '}';
    }
    return result + "]}";
}

[[nodiscard]] std::string BlockJson(const ExplorerBlock& block,
                                    const primitives::TransactionLimits& limits)
{
    std::string result{
        "{\"height\":" + std::to_string(block.height) + ",\"hash\":\"" + HashHex(block.hash) +
        "\",\"time\":" + std::to_string(block.block.header.time) +
        ",\"bits\":" + std::to_string(block.block.header.bits) + ",\"transactions\":["};
    for (std::size_t index = 0U; index < block.block.transactions.size(); ++index) {
        const auto transaction_id = block.block.transactions.at(index).TxId(limits);
        if (!transaction_id.has_value()) {
            return "{}";
        }
        if (index != 0U) {
            result += ',';
        }
        result += "\"" + HashHex(*transaction_id) + "\"";
    }
    return result + "]}";
}

} // namespace

ExplorerHttpService::ExplorerHttpService(ExplorerHttpConfig config,
                                         const ExplorerIndex& index) noexcept
    : config_(std::move(config)), index_(index)
{
}

std::unique_ptr<ExplorerHttpService>
ExplorerHttpService::Create(ExplorerHttpConfig config, const ExplorerIndex& index) noexcept
{
    if (!IsLoopback(config.bind_address) || !IsValidCredential(config.username, true) ||
        !IsValidCredential(config.password, false) || config.network == nullptr ||
        consensus::CheckNetworkParams(*config.network) != consensus::NetworkParamsError::kNone ||
        config.limits.max_target_bytes == 0U || config.limits.max_authorization_bytes == 0U ||
        config.limits.max_response_items == 0U) {
        return nullptr;
    }
    try {
        return std::unique_ptr<ExplorerHttpService>{
            new ExplorerHttpService{std::move(config), index}};
    } catch (...) {
        return nullptr;
    }
}

ExplorerHttpResponse ExplorerHttpService::Handle(const ExplorerHttpRequest& request) const noexcept
{
    try {
        if (request.target.size() > config_.limits.max_target_bytes ||
            request.authorization.size() > config_.limits.max_authorization_bytes ||
            !request.body.empty()) {
            return Error(400U, "malformed_request");
        }
        if (request.method != "GET") {
            return Error(405U, "method_not_allowed");
        }
        const auto decoded = DecodeBasic(request.authorization);
        const auto expected = config_.username + ":" + config_.password;
        if (!decoded.has_value() || !ConstantTimeEqual(*decoded, expected)) {
            return Error(401U, "unauthorized");
        }

        // A snapshot must never be rendered with a different network's
        // address prefix. This also prevents a stale relay misconfiguration
        // from presenting REGTEST data as TESTNET data.
        const auto summary = index_.Summary();
        if (!summary.has_value()) {
            return Error(404U, "index_unavailable");
        }
        if (summary->network != config_.network->id) {
            return Error(409U, "index_network_mismatch");
        }

        if (request.target == "/api/v1/summary") {
            return {200U, "application/json",
                    std::string{"{\"network\":\""} + NetworkName(config_.network->id) +
                        "\",\"notice\":\"" +
                        (config_.network->id == consensus::NetworkId::kTestnet
                             ? "NOVA TESTNET — COINS HAVE NO VALUE"
                             : "") +
                        "\",\"height\":" + std::to_string(summary->height) + ",\"best_block\":\"" +
                        HashHex(summary->best_block) + "\",\"difficulty_target\":\"" +
                        Hex(summary->target.bytes()) + "\",\"target_spacing_seconds\":" +
                        std::to_string(summary->target_spacing_seconds) +
                        ",\"estimated_blocks_per_day\":" +
                        std::to_string(summary->estimated_blocks_per_day) + ",\"indexed_blocks\":" +
                        std::to_string(summary->indexed_blocks) + ",\"indexed_transactions\":" +
                        std::to_string(summary->indexed_transactions) +
                        ",\"unspent_outputs\":" + std::to_string(summary->unspent_outputs) + "}"};
        }

        constexpr std::string_view kLatest{"/api/v1/blocks/latest"};
        if (request.target.starts_with(kLatest)) {
            const auto limit = ParseLimit(request.target.substr(kLatest.size()),
                                          config_.limits.max_response_items);
            const auto transaction_limits = index_.TransactionLimits();
            if (!limit.has_value() || !transaction_limits.has_value()) {
                return Error(400U, "invalid_limit");
            }
            std::string body{"["};
            for (const auto& block : index_.LatestBlocks(*limit)) {
                if (body.size() != 1U) {
                    body += ',';
                }
                body += BlockJson(block, *transaction_limits);
            }
            return {200U, "application/json", body + "]"};
        }

        constexpr std::string_view kHeight{"/api/v1/blocks/height/"};
        if (request.target.starts_with(kHeight)) {
            std::uint32_t height{};
            const auto text = request.target.substr(kHeight.size());
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), height);
            if (text.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != text.data() + text.size()) {
                return Error(400U, "invalid_height");
            }
            const auto block = index_.FindBlockByHeight(height);
            const auto transaction_limits = index_.TransactionLimits();
            return block.has_value() && transaction_limits.has_value()
                       ? ExplorerHttpResponse{200U, "application/json",
                                              BlockJson(*block, *transaction_limits)}
                       : Error(404U, "not_found");
        }

        constexpr std::string_view kBlockHash{"/api/v1/blocks/hash/"};
        if (request.target.starts_with(kBlockHash)) {
            const auto hash = crypto::Hash256::FromHex(request.target.substr(kBlockHash.size()));
            const auto block = hash.has_value() ? index_.FindBlockByHash(*hash) : std::nullopt;
            const auto transaction_limits = index_.TransactionLimits();
            return block.has_value() && transaction_limits.has_value()
                       ? ExplorerHttpResponse{200U, "application/json",
                                              BlockJson(*block, *transaction_limits)}
                       : Error(404U, "not_found");
        }

        constexpr std::string_view kTransaction{"/api/v1/transactions/"};
        if (request.target.starts_with(kTransaction)) {
            const auto hash = crypto::Hash256::FromHex(request.target.substr(kTransaction.size()));
            const auto transaction =
                hash.has_value() ? index_.FindTransaction(*hash) : std::nullopt;
            return transaction.has_value()
                       ? ExplorerHttpResponse{200U, "application/json",
                                              TransactionJson(*transaction, *config_.network)}
                       : Error(404U, "not_found");
        }

        constexpr std::string_view kAddress{"/api/v1/addresses/"};
        constexpr std::string_view kAddressSuffix{"/utxos"};
        if (request.target.starts_with(kAddress)) {
            const auto remainder = request.target.substr(kAddress.size());
            const auto suffix_offset = remainder.find(kAddressSuffix);
            if (suffix_offset == std::string_view::npos ||
                remainder.substr(suffix_offset, kAddressSuffix.size()) != kAddressSuffix) {
                return Error(400U, "invalid_address_route");
            }
            const auto text = remainder.substr(0U, suffix_offset);
            const auto address = wallet::DecodeP2pkhAddress(text, *config_.network);
            if (!address.has_value()) {
                return Error(400U, "invalid_address");
            }
            const auto limit = ParseLimit(remainder.substr(suffix_offset + kAddressSuffix.size()),
                                          config_.limits.max_response_items);
            if (!limit.has_value()) {
                return Error(400U, "invalid_limit");
            }
            const auto utxos = index_.FindUtxosByAddress(*address, *limit);
            std::string body{"["};
            for (const auto& utxo : utxos) {
                if (body.size() != 1U) {
                    body += ',';
                }
                body += UtxoJson(utxo, *config_.network);
            }
            return {200U, "application/json", body + "]"};
        }

        constexpr std::string_view kUtxos{"/api/v1/utxos"};
        if (request.target.starts_with(kUtxos)) {
            const auto limit =
                ParseLimit(request.target.substr(kUtxos.size()), config_.limits.max_response_items);
            if (!limit.has_value()) {
                return Error(400U, "invalid_limit");
            }
            std::string body{"["};
            for (const auto& utxo : index_.AllUtxos(*limit)) {
                if (body.size() != 1U) {
                    body += ',';
                }
                body += UtxoJson(utxo, *config_.network);
            }
            return {200U, "application/json", body + "]"};
        }
        return Error(404U, "not_found");
    } catch (...) {
        return Error(500U, "internal_error");
    }
}

const std::string& ExplorerHttpService::bind_address() const noexcept
{
    return config_.bind_address;
}

} // namespace nova::explorer
