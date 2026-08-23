#include "rpc/rpc.hpp"

#include "wallet/address.hpp"

#include "consensus/network_params.hpp"
#include "consensus/pow.hpp"
#include "primitives/serialization.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nova::rpc
{
namespace
{

constexpr std::int32_t kInvalidRequest = -32600;
constexpr std::int32_t kMethodNotFound = -32601;
constexpr std::int32_t kInvalidParams = -32602;
constexpr std::int32_t kAuthenticationFailure = -32001;
constexpr std::int32_t kNotFound = -32002;
constexpr std::int32_t kWalletFailure = -32003;
constexpr std::int32_t kNodeFailure = -32004;
constexpr std::int32_t kRegtestOnly = -32005;
constexpr std::size_t kMaximumExplorerSnapshotBytes = 32U * 1024U * 1024U;

[[nodiscard]] bool IsLoopback(const std::string_view address) noexcept
{
    return address == "127.0.0.1" || address == "::1";
}

[[nodiscard]] bool IsRegtest(const consensus::NetworkId network) noexcept
{
    return network == consensus::NetworkId::kRegtest;
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

[[nodiscard]] std::string JsonEscape(const std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 2U);
    for (const auto character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

[[nodiscard]] std::string ErrorResponse(const std::string_view id, const std::int32_t code,
                                        const std::string_view message)
{
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string{id} +
           ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" +
           JsonEscape(message) + "\"}}";
}

[[nodiscard]] std::string ResultResponse(const std::string_view id, const std::string_view result)
{
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::string{id} + ",\"result\":" + std::string{result} +
           "}";
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

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
DecodeHex(const std::string_view text, const std::size_t max_bytes) noexcept
{
    if (text.empty() || (text.size() % 2U) != 0U || text.size() / 2U > max_bytes) {
        return std::nullopt;
    }
    try {
        std::vector<std::uint8_t> result;
        result.reserve(text.size() / 2U);
        for (std::size_t index = 0U; index < text.size(); index += 2U) {
            const auto decode = [](const char character) -> std::optional<std::uint8_t> {
                if (character >= '0' && character <= '9') {
                    return static_cast<std::uint8_t>(character - '0');
                }
                if (character >= 'a' && character <= 'f') {
                    return static_cast<std::uint8_t>(character - 'a' + 10);
                }
                if (character >= 'A' && character <= 'F') {
                    return static_cast<std::uint8_t>(character - 'A' + 10);
                }
                return std::nullopt;
            };
            const auto high = decode(text[index]);
            const auto low = decode(text[index + 1U]);
            if (!high.has_value() || !low.has_value()) {
                return std::nullopt;
            }
            result.push_back(static_cast<std::uint8_t>((*high << 4U) | *low));
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
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
        std::string decoded;
        decoded.reserve((encoded.size() / 4U) * 3U);
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

[[nodiscard]] std::size_t SkipWhitespace(const std::string_view text, std::size_t position) noexcept
{
    while (position < text.size() &&
           std::isspace(static_cast<unsigned char>(text[position])) != 0) {
        ++position;
    }
    return position;
}

[[nodiscard]] std::optional<std::size_t> EndOfString(const std::string_view text,
                                                     std::size_t position) noexcept
{
    if (position >= text.size() || text[position] != '"') {
        return std::nullopt;
    }
    ++position;
    while (position < text.size()) {
        const auto character = text[position++];
        if (character == '\\') {
            if (position >= text.size()) {
                return std::nullopt;
            }
            ++position;
        } else if (character == '"') {
            return position;
        } else if (static_cast<unsigned char>(character) < 0x20U) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> EndOfValue(const std::string_view text,
                                                    std::size_t position) noexcept
{
    position = SkipWhitespace(text, position);
    if (position >= text.size()) {
        return std::nullopt;
    }
    if (text[position] == '"') {
        return EndOfString(text, position);
    }
    if (text[position] == '{' || text[position] == '[') {
        const auto opening = text[position];
        const auto closing = opening == '{' ? '}' : ']';
        std::size_t depth = 0U;
        while (position < text.size()) {
            if (text[position] == '"') {
                const auto string_end = EndOfString(text, position);
                if (!string_end.has_value()) {
                    return std::nullopt;
                }
                position = *string_end;
                continue;
            }
            if (text[position] == opening) {
                ++depth;
            } else if (text[position] == closing && --depth == 0U) {
                return position + 1U;
            }
            ++position;
        }
        return std::nullopt;
    }
    while (position < text.size() && text[position] != ',' && text[position] != '}' &&
           text[position] != ']') {
        if (std::isspace(static_cast<unsigned char>(text[position])) != 0) {
            break;
        }
        ++position;
    }
    return position == 0U ? std::nullopt : std::optional<std::size_t>{position};
}

[[nodiscard]] std::optional<std::string_view> Field(const std::string_view object,
                                                    const std::string_view wanted) noexcept
{
    if (object.size() < 2U || object.front() != '{' || object.back() != '}') {
        return std::nullopt;
    }
    std::optional<std::string_view> found;
    std::size_t position = 1U;
    while (true) {
        position = SkipWhitespace(object, position);
        if (position == object.size() - 1U) {
            return found;
        }
        const auto key_end = EndOfString(object, position);
        if (!key_end.has_value() || *key_end < position + 2U) {
            return std::nullopt;
        }
        const auto key = object.substr(position + 1U, *key_end - position - 2U);
        position = SkipWhitespace(object, *key_end);
        if (position >= object.size() || object[position++] != ':') {
            return std::nullopt;
        }
        position = SkipWhitespace(object, position);
        const auto value_end = EndOfValue(object, position);
        if (!value_end.has_value() || *value_end <= position) {
            return std::nullopt;
        }
        if (key == wanted) {
            if (found.has_value()) {
                return std::nullopt;
            }
            found = object.substr(position, *value_end - position);
        }
        position = SkipWhitespace(object, *value_end);
        if (position >= object.size()) {
            return std::nullopt;
        }
        if (object[position] == '}') {
            return position == object.size() - 1U ? found : std::nullopt;
        }
        if (object[position++] != ',') {
            return std::nullopt;
        }
    }
}

[[nodiscard]] std::optional<std::string_view> JsonString(const std::string_view raw) noexcept
{
    const auto end = EndOfString(raw, 0U);
    if (!end.has_value() || *end != raw.size()) {
        return std::nullopt;
    }
    // Escaped names/values are rejected rather than ambiguously decoded.
    const auto value = raw.substr(1U, raw.size() - 2U);
    return value.find('\\') == std::string_view::npos ? std::optional<std::string_view>{value}
                                                      : std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> JsonInteger(const std::string_view raw) noexcept
{
    std::int64_t value{};
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string AmountJson(const primitives::Amount amount)
{
    return std::to_string(amount);
}

[[nodiscard]] std::optional<std::string>
TransactionHex(const primitives::Transaction& transaction,
               const primitives::TransactionLimits& limits) noexcept
{
    primitives::BinaryWriter writer;
    if (!transaction.Serialize(writer, limits)) {
        return std::nullopt;
    }
    return Hex(writer.bytes());
}

[[nodiscard]] std::optional<primitives::BlockHeader>
HeaderFromBytes(const std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() != 80U) {
        return std::nullopt;
    }
    primitives::BinaryReader reader{bytes};
    const auto version = reader.ReadU32();
    const auto previous = reader.ReadHash256();
    const auto merkle = reader.ReadHash256();
    const auto time = reader.ReadU32();
    const auto bits = reader.ReadU32();
    const auto nonce = reader.ReadU32();
    if (!version.has_value() || !previous.has_value() || !merkle.has_value() || !time.has_value() ||
        !bits.has_value() || !nonce.has_value() || !reader.RequireEnd()) {
        return std::nullopt;
    }
    return primitives::BlockHeader{
        std::bit_cast<std::int32_t>(*version), *previous, *merkle, *time, *bits, *nonce};
}

[[nodiscard]] std::optional<std::string>
ExplorerSnapshotHex(const chain::ChainState& chain) noexcept
{
    const auto active = chain.GetActiveBlockIndexes();
    const auto& network = consensus::RegtestNetworkParams();
    if (!active.has_value() || active->empty()) {
        return std::nullopt;
    }
    try {
        primitives::BinaryWriter writer;
        if (!writer.WriteU32(1U) || !writer.WriteCompactSize(active->size())) {
            return std::nullopt;
        }
        for (const auto& index : *active) {
            const auto block = index.height == 0U
                                   ? std::optional<primitives::Block>{network.genesis_block}
                                   : index.block;
            if (!block.has_value()) {
                return std::nullopt;
            }
            primitives::BinaryWriter encoded_block;
            if (!block->Serialize(encoded_block, network.block_limits) ||
                !writer.WriteU32(index.height) ||
                !writer.WriteBytes(encoded_block.bytes(),
                                   network.block_limits.max_serialized_size) ||
                writer.bytes().size() > kMaximumExplorerSnapshotBytes) {
                return std::nullopt;
            }
        }
        return Hex(writer.bytes());
    } catch (...) {
        return std::nullopt;
    }
}

class MempoolBroadcaster final : public wallet::TransactionBroadcaster
{
  public:
    MempoolBroadcaster(chain::Mempool& mempool, chain::UTXOSet& utxos,
                       const chain::TransactionValidationContext context,
                       const std::uint64_t now) noexcept
        : mempool_(mempool), utxos_(utxos), context_(context), now_(now)
    {
    }

    wallet::BroadcastResult Broadcast(const primitives::Transaction& transaction) override
    {
        const auto result = mempool_.AcceptToMempool(transaction, context_, utxos_, now_);
        return wallet::BroadcastResult{result.error == chain::MempoolError::kNone};
    }

  private:
    chain::Mempool& mempool_;
    chain::UTXOSet& utxos_;
    chain::TransactionValidationContext context_;
    std::uint64_t now_{};
};

} // namespace

RpcService::RpcService(RpcConfig config, RpcDependencies dependencies) noexcept
    : config_(std::move(config)), dependencies_(std::move(dependencies))
{
}

std::unique_ptr<RpcService> RpcService::Create(RpcConfig config,
                                               RpcDependencies dependencies) noexcept
{
    if (!IsLoopback(config.bind_address) || config.username.empty() || config.password.empty() ||
        config.limits.max_header_bytes == 0U || config.limits.max_body_bytes == 0U ||
        config.limits.max_mining_attempts == 0U) {
        return nullptr;
    }
    try {
        return std::unique_ptr<RpcService>{
            new RpcService{std::move(config), std::move(dependencies)}};
    } catch (...) {
        return nullptr;
    }
}

const std::string& RpcService::bind_address() const noexcept
{
    return config_.bind_address;
}

HttpResponse RpcService::HandleHttpPost(const HttpRequest& request) noexcept
{
    constexpr std::string_view kUnknownId{"null"};
    if (request.method != "POST" || request.body.size() > config_.limits.max_body_bytes ||
        request.authorization.size() > config_.limits.max_header_bytes) {
        return {400U, ErrorResponse(kUnknownId, kInvalidRequest, "invalid HTTP request")};
    }
    const auto credentials = DecodeBasic(request.authorization);
    const auto expected = config_.username + ":" + config_.password;
    if (!credentials.has_value() || !ConstantTimeEqual(*credentials, expected)) {
        return {401U, ErrorResponse(kUnknownId, kAuthenticationFailure, "authentication failed")};
    }
    const auto version = Field(request.body, "jsonrpc");
    const auto method_raw = Field(request.body, "method");
    const auto id_raw = Field(request.body, "id");
    const auto params = Field(request.body, "params");
    const auto version_text = version.has_value() ? JsonString(*version) : std::nullopt;
    const auto method = method_raw.has_value() ? JsonString(*method_raw) : std::nullopt;
    if (!version_text.has_value() || *version_text != "2.0" || !method.has_value() ||
        !id_raw.has_value() || *id_raw == "null") {
        return {400U, ErrorResponse(kUnknownId, kInvalidRequest, "invalid JSON-RPC request")};
    }
    const auto id = *id_raw;
    const auto parameter =
        [params](const std::string_view name) -> std::optional<std::string_view> {
        return !params.has_value() ? std::nullopt : Field(*params, name);
    };
    try {
        if (*method == "getblockchaininfo") {
            return {200U, ResultResponse(
                              id, "{\"blocks\":" +
                                      std::to_string(dependencies_.chain_state.active_height()) +
                                      ",\"bestblockhash\":\"" +
                                      Hex(dependencies_.chain_state.active_tip().bytes()) +
                                      "\",\"chain\":\"" + NetworkName(config_.network) + "\"}")};
        }
        if (*method == "getnodehealth" || *method == "getnodemetrics") {
            const auto metrics = dependencies_.read_metrics ? dependencies_.read_metrics()
                                                            : observability::MetricsSnapshot{};
            const auto counters =
                "\"uptime_seconds\":" + std::to_string(metrics.uptime_seconds) +
                ",\"last_block_arrival_time_seconds\":" +
                std::to_string(metrics.last_block_arrival_time_seconds) +
                ",\"blocks_accepted\":" + std::to_string(metrics.blocks_accepted) +
                ",\"blocks_rejected\":" + std::to_string(metrics.blocks_rejected) +
                ",\"transactions_accepted\":" + std::to_string(metrics.transactions_accepted) +
                ",\"transactions_rejected\":" + std::to_string(metrics.transactions_rejected) +
                ",\"p2p_messages_dispatched\":" + std::to_string(metrics.p2p_messages_dispatched) +
                ",\"p2p_disconnects\":" + std::to_string(metrics.p2p_disconnects) +
                ",\"p2p_transport_errors\":" + std::to_string(metrics.p2p_transport_errors) +
                ",\"validation_failures\":" +
                std::to_string(observability::SaturatingSum(metrics.blocks_rejected,
                                                            metrics.transactions_rejected));
            if (*method == "getnodemetrics") {
                return {200U, ResultResponse(id, "{" + counters + "}")};
            }
            return {
                200U,
                ResultResponse(
                    id,
                    "{\"ready\":true,\"network\":\"" + std::string{NetworkName(config_.network)} +
                        "\",\"height\":" +
                        std::to_string(dependencies_.chain_state.active_height()) +
                        ",\"bestblockhash\":\"" +
                        Hex(dependencies_.chain_state.active_tip().bytes()) +
                        "\",\"chainwork\":\"" + [&]()
                        -> std::string {
                        const auto index = dependencies_.chain_state.GetBlockIndex(
                            dependencies_.chain_state.active_tip());
                        return index.has_value() ? Hex(index->chain_work.bytes()) : std::string{};
                    }() + "\",\"mempool_bytes\":" +
                               std::to_string(dependencies_.mempool.total_serialized_size()) +
                               ",\"peers\":" + std::to_string(dependencies_.peers.peer_count()) +
                               ",\"mempool\":" + std::to_string(dependencies_.mempool.size()) +
                               "," + counters + "}")};
        }
        if (*method == "getexplorersnapshot") {
            if (config_.network == consensus::NetworkId::kMainnet) {
                return {200U, ErrorResponse(id, kRegtestOnly, "snapshot network unavailable")};
            }
            const auto snapshot = ExplorerSnapshotHex(dependencies_.chain_state);
            if (!snapshot.has_value()) {
                return {200U, ErrorResponse(id, kNodeFailure, "snapshot generation failed")};
            }
            return {200U, ResultResponse(id, "{\"encoding\":\"nova-snapshot-v1\",\"data\":\"" +
                                                 *snapshot + "\"}")};
        }
        if (*method == "getblock") {
            const auto hash_raw = parameter("hash");
            const auto hash_text = hash_raw.has_value() ? JsonString(*hash_raw) : std::nullopt;
            const auto hash =
                hash_text.has_value() ? crypto::Hash256::FromHex(*hash_text) : std::nullopt;
            const auto index =
                hash.has_value() ? dependencies_.chain_state.GetBlockIndex(*hash) : std::nullopt;
            if (!index.has_value()) {
                return {200U, ErrorResponse(id, kNotFound, "block not found")};
            }
            return {200U,
                    ResultResponse(
                        id, "{\"hash\":\"" + Hex(index->hash.bytes()) +
                                "\",\"height\":" + std::to_string(index->height) +
                                ",\"time\":" + std::to_string(index->time) + ",\"active\":" +
                                (index->status == chain::BlockStatus::kActive ? "true" : "false") +
                                ",\"transactions\":" +
                                std::to_string(index->block.has_value()
                                                   ? index->block->transactions.size()
                                                   : 0U) +
                                "}")};
        }
        if (*method == "gettransaction" || *method == "getmempoolentry") {
            const auto hash_raw = parameter("txid");
            const auto hash_text = hash_raw.has_value() ? JsonString(*hash_raw) : std::nullopt;
            const auto hash =
                hash_text.has_value() ? crypto::Hash256::FromHex(*hash_text) : std::nullopt;
            const auto entry =
                hash.has_value() ? dependencies_.mempool.GetEntry(*hash) : std::nullopt;
            if (!entry.has_value()) {
                return {200U, ErrorResponse(id, kNotFound, "transaction not found")};
            }
            if (*method == "getmempoolentry") {
                return {200U,
                        ResultResponse(
                            id, "{\"fee\":" + AmountJson(entry->fee) +
                                    ",\"size\":" + std::to_string(entry->serialized_size) +
                                    ",\"time\":" + std::to_string(entry->time_received) + "}")};
            }
            const auto serialized =
                TransactionHex(entry->transaction, dependencies_.wallet.transaction_limits());
            if (!serialized.has_value()) {
                return {200U, ErrorResponse(id, kNodeFailure, "transaction serialization failed")};
            }
            return {200U, ResultResponse(id, "{\"hex\":\"" + *serialized + "\"}")};
        }
        if (*method == "getutxo") {
            const auto hash_raw = parameter("txid");
            const auto output_raw = parameter("vout");
            const auto hash_text = hash_raw.has_value() ? JsonString(*hash_raw) : std::nullopt;
            const auto output = output_raw.has_value() ? JsonInteger(*output_raw) : std::nullopt;
            const auto hash =
                hash_text.has_value() ? crypto::Hash256::FromHex(*hash_text) : std::nullopt;
            if (!hash.has_value() || !output.has_value() || *output < 0 ||
                static_cast<std::uint64_t>(*output) > std::numeric_limits<std::uint32_t>::max()) {
                return {200U, ErrorResponse(id, kInvalidParams, "txid and vout required")};
            }
            const auto coin = dependencies_.utxos.GetCoin(
                chain::UTXOKey{*hash, static_cast<std::uint32_t>(*output)});
            if (!coin.has_value()) {
                return {200U, ErrorResponse(id, kNotFound, "UTXO not found")};
            }
            return {200U, ResultResponse(id, "{\"amount\":" + AmountJson(coin->output.value) +
                                                 ",\"height\":" + std::to_string(coin->height) +
                                                 ",\"coinbase\":" +
                                                 (coin->is_coinbase ? "true" : "false") + "}")};
        }
        if (*method == "getmempoolinfo") {
            return {200U,
                    ResultResponse(
                        id, "{\"size\":" + std::to_string(dependencies_.mempool.size()) +
                                ",\"bytes\":" +
                                std::to_string(dependencies_.mempool.total_serialized_size()) +
                                "}")};
        }
        if (*method == "getpeerinfo") {
            return {200U,
                    ResultResponse(id, "{\"count\":" +
                                           std::to_string(dependencies_.peers.peer_count()) + "}")};
        }
        if (*method == "connectpeer") {
            const auto host_raw = parameter("host");
            const auto port_raw = parameter("port");
            const auto host = host_raw.has_value() ? JsonString(*host_raw) : std::nullopt;
            const auto port = port_raw.has_value() ? JsonInteger(*port_raw) : std::nullopt;
            if (!IsRegtest(config_.network) || !host.has_value() ||
                (*host != "127.0.0.1" && *host != "::1") || !port.has_value() || *port <= 0 ||
                static_cast<std::uint64_t>(*port) > std::numeric_limits<std::uint16_t>::max() ||
                !dependencies_.connect_peer ||
                !dependencies_.connect_peer(
                    {std::string{*host},
                     static_cast<std::uint16_t>(static_cast<std::uint64_t>(*port))})) {
                return {200U, ErrorResponse(id, kNodeFailure, "peer connection failed")};
            }
            return {200U, ResultResponse(id, "true")};
        }
        if (*method == "disconnectpeers") {
            if (!IsRegtest(config_.network) || !dependencies_.disconnect_peers) {
                return {200U, ErrorResponse(id, kRegtestOnly, "peer control unavailable")};
            }
            dependencies_.disconnect_peers();
            return {200U, ResultResponse(id, "true")};
        }
        if (*method == "getnewaddress") {
            const auto address = dependencies_.wallet.GenerateReceivingAddress();
            if (!address.has_value() ||
                (dependencies_.persist_wallet && !dependencies_.persist_wallet())) {
                return {200U, ErrorResponse(id, kWalletFailure, "address generation failed")};
            }
            const auto encoded = wallet::EncodeP2pkhAddress(
                address->public_key_hash, consensus::GetNetworkParams(config_.network));
            if (!encoded.has_value()) {
                return {200U, ErrorResponse(id, kWalletFailure, "address encoding failed")};
            }
            return {200U, ResultResponse(id, "\"" + *encoded + "\"")};
        }
        if (*method == "getbalances") {
            return {200U, ResultResponse(
                              id, "{\"confirmed\":" +
                                      AmountJson(dependencies_.wallet.ConfirmedBalance()) +
                                      ",\"unconfirmed\":" +
                                      AmountJson(dependencies_.wallet.UnconfirmedBalance()) + "}")};
        }
        if (*method == "listunspent") {
            const auto coins = dependencies_.wallet.ListUTXOs();
            std::string result{"["};
            for (std::size_t index = 0U; index < coins.size(); ++index) {
                if (index != 0U) {
                    result += ',';
                }
                result += "{\"txid\":\"" + Hex(coins[index].key.transaction_id.bytes()) +
                          "\",\"vout\":" + std::to_string(coins[index].key.output_index) +
                          ",\"amount\":" + AmountJson(coins[index].output.value) +
                          ",\"confirmed\":" + (coins[index].confirmed ? "true" : "false") + "}";
            }
            result += ']';
            return {200U, ResultResponse(id, result)};
        }
        if (*method == "createtransaction") {
            const auto address_raw = parameter("address");
            const auto amount_raw = parameter("amount");
            const auto address = address_raw.has_value() ? JsonString(*address_raw) : std::nullopt;
            const auto amount = amount_raw.has_value() ? JsonInteger(*amount_raw) : std::nullopt;
            const auto key_hash = address.has_value()
                                      ? wallet::DecodeP2pkhAddress(
                                            *address, consensus::GetNetworkParams(config_.network))
                                      : std::nullopt;
            if (!amount.has_value() || !key_hash.has_value()) {
                return {200U,
                        ErrorResponse(id, kInvalidParams, "address and integer amount required")};
            }
            const std::array<wallet::Recipient, 1U> recipients{
                {wallet::Recipient{*key_hash, *amount}}};
            const auto built = dependencies_.wallet.CreateTransaction(recipients);
            if (!built.built.has_value()) {
                return {200U, ErrorResponse(id, kWalletFailure, "transaction construction failed")};
            }
            const auto serialized =
                TransactionHex(built.built->transaction, dependencies_.wallet.transaction_limits());
            if (!serialized.has_value()) {
                return {200U, ErrorResponse(id, kNodeFailure, "transaction serialization failed")};
            }
            return {200U, ResultResponse(id, "{\"hex\":\"" + *serialized + "\",\"fee\":" +
                                                 AmountJson(built.built->fee) + "}")};
        }
        if (*method == "sendtoaddress") {
            const auto address_raw = parameter("address");
            const auto amount_raw = parameter("amount");
            const auto address = address_raw.has_value() ? JsonString(*address_raw) : std::nullopt;
            const auto amount = amount_raw.has_value() ? JsonInteger(*amount_raw) : std::nullopt;
            const auto key_hash = address.has_value()
                                      ? wallet::DecodeP2pkhAddress(
                                            *address, consensus::GetNetworkParams(config_.network))
                                      : std::nullopt;
            if (!IsRegtest(config_.network) || !amount.has_value() || !key_hash.has_value() ||
                !dependencies_.send_to_address) {
                return {200U,
                        ErrorResponse(id, kInvalidParams, "address and integer amount required")};
            }
            const auto transaction_id = dependencies_.send_to_address({*key_hash, *amount});
            if (!transaction_id.has_value()) {
                return {200U, ErrorResponse(id, kNodeFailure, "transaction broadcast failed")};
            }
            return {200U, ResultResponse(id, "\"" + Hex(transaction_id->bytes()) + "\"")};
        }
        if (*method == "signtransaction" || *method == "sendwallettransaction") {
            const auto hex_raw = parameter("hex");
            const auto hex = hex_raw.has_value() ? JsonString(*hex_raw) : std::nullopt;
            const auto encoded =
                hex.has_value()
                    ? DecodeHex(*hex, dependencies_.wallet.transaction_limits().max_serialized_size)
                    : std::nullopt;
            const auto transaction = encoded.has_value()
                                         ? primitives::Transaction::Deserialize(
                                               *encoded, dependencies_.wallet.transaction_limits())
                                         : std::nullopt;
            if (!transaction.has_value()) {
                return {200U, ErrorResponse(id, kInvalidParams, "invalid transaction hex")};
            }
            if (*method == "signtransaction") {
                const auto signed_transaction = dependencies_.wallet.SignTransaction(*transaction);
                if (!signed_transaction.transaction.has_value()) {
                    return {200U, ErrorResponse(id, kWalletFailure, "transaction signing failed")};
                }
                const auto serialized = TransactionHex(*signed_transaction.transaction,
                                                       dependencies_.wallet.transaction_limits());
                if (!serialized.has_value()) {
                    return {200U,
                            ErrorResponse(id, kNodeFailure, "transaction serialization failed")};
                }
                return {200U, ResultResponse(id, "{\"hex\":\"" + *serialized + "\"}")};
            }
            MempoolBroadcaster broadcaster{dependencies_.mempool, dependencies_.utxos,
                                           dependencies_.transaction_context,
                                           dependencies_.current_time};
            const auto result = dependencies_.wallet.Broadcast(*transaction, broadcaster);
            if (result != wallet::WalletError::kNone) {
                return {200U, ErrorResponse(id, kNodeFailure, "transaction broadcast failed")};
            }
            const auto txid = transaction->TxId(dependencies_.wallet.transaction_limits());
            if (!txid.has_value()) {
                return {200U, ErrorResponse(id, kNodeFailure, "transaction ID calculation failed")};
            }
            return {200U, ResultResponse(id, "\"" + Hex(txid->bytes()) + "\"")};
        }
        if (*method == "mineregtestheader") {
            if (!IsRegtest(config_.network)) {
                return {200U, ErrorResponse(id, kRegtestOnly, "regtest RPC disabled")};
            }
            const auto header_raw = parameter("header");
            const auto attempts_raw = parameter("max_attempts");
            const auto header_text =
                header_raw.has_value() ? JsonString(*header_raw) : std::nullopt;
            const auto attempts =
                attempts_raw.has_value() ? JsonInteger(*attempts_raw) : std::nullopt;
            if (!header_text.has_value() || !attempts.has_value() || *attempts <= 0 ||
                static_cast<std::uint64_t>(*attempts) > config_.limits.max_mining_attempts) {
                return {200U,
                        ErrorResponse(id, kInvalidParams, "invalid regtest mining parameters")};
            }
            const auto bytes = DecodeHex(*header_text, 80U);
            const auto header = bytes.has_value() ? HeaderFromBytes(*bytes) : std::nullopt;
            if (!header.has_value()) {
                return {200U, ErrorResponse(id, kInvalidParams, "invalid serialized block header")};
            }
            const auto mined =
                consensus::MineRegtestBlock(*header, static_cast<std::uint64_t>(*attempts));
            if (!mined.has_value()) {
                return {200U, ErrorResponse(id, kNodeFailure, "no proof of work found")};
            }
            primitives::BinaryWriter writer;
            if (!writer.WriteU32(std::bit_cast<std::uint32_t>(mined->header.version)) ||
                !writer.WriteHash256(mined->header.previous_block_id) ||
                !writer.WriteHash256(mined->header.merkle_root) ||
                !writer.WriteU32(mined->header.time) || !writer.WriteU32(mined->header.bits) ||
                !writer.WriteU32(mined->header.nonce)) {
                return {200U, ErrorResponse(id, kNodeFailure, "header serialization failed")};
            }
            return {200U, ResultResponse(
                              id, "{\"hash\":\"" + Hex(mined->block_hash.bytes()) +
                                      "\",\"header\":\"" + Hex(writer.bytes()) +
                                      "\",\"attempts\":" + std::to_string(mined->attempts) + "}")};
        }
        if (*method == "generateregtestblock") {
            if (!IsRegtest(config_.network) || !dependencies_.mine_regtest_block) {
                return {200U,
                        ErrorResponse(id, kRegtestOnly, "regtest block generation unavailable")};
            }
            const auto block_hash = dependencies_.mine_regtest_block();
            if (!block_hash.has_value()) {
                return {200U, ErrorResponse(id, kNodeFailure, "regtest block generation failed")};
            }
            return {200U, ResultResponse(id, "\"" + Hex(block_hash->bytes()) + "\"")};
        }
        return {200U, ErrorResponse(id, kMethodNotFound, "method not found")};
    } catch (const std::bad_alloc&) {
        return {500U, ErrorResponse(id, kNodeFailure, "allocation failure")};
    } catch (...) {
        return {500U, ErrorResponse(id, kNodeFailure, "internal RPC failure")};
    }
}

} // namespace nova::rpc
