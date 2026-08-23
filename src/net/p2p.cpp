#include "net/p2p.hpp"

#include "crypto/crypto.hpp"
#include "primitives/serialization.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace nova::net
{
namespace
{

[[nodiscard]] bool WriteI32(primitives::BinaryWriter& writer, const std::int32_t value) noexcept
{
    return writer.WriteU32(std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] std::optional<std::int32_t> ReadI32(primitives::BinaryReader& reader) noexcept
{
    const auto value = reader.ReadU32();
    return value.has_value() ? std::optional<std::int32_t>{std::bit_cast<std::int32_t>(*value)}
                             : std::nullopt;
}

[[nodiscard]] bool WriteU16BigEndian(primitives::BinaryWriter& writer,
                                     const std::uint16_t value) noexcept
{
    return writer.WriteU8(static_cast<std::uint8_t>(value >> 8U)) &&
           writer.WriteU8(static_cast<std::uint8_t>(value));
}

[[nodiscard]] std::optional<std::uint16_t>
ReadU16BigEndian(primitives::BinaryReader& reader) noexcept
{
    const auto high = reader.ReadU8();
    const auto low = reader.ReadU8();
    if (!high.has_value() || !low.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(*high) << 8U) | *low);
}

[[nodiscard]] bool WriteHeader(primitives::BinaryWriter& writer,
                               const primitives::BlockHeader& header) noexcept
{
    return WriteI32(writer, header.version) && writer.WriteHash256(header.previous_block_id) &&
           writer.WriteHash256(header.merkle_root) && writer.WriteU32(header.time) &&
           writer.WriteU32(header.bits) && writer.WriteU32(header.nonce);
}

[[nodiscard]] std::optional<primitives::BlockHeader>
ReadHeader(primitives::BinaryReader& reader) noexcept
{
    const auto version = ReadI32(reader);
    const auto previous = reader.ReadHash256();
    const auto merkle = reader.ReadHash256();
    const auto time = reader.ReadU32();
    const auto bits = reader.ReadU32();
    const auto nonce = reader.ReadU32();
    if (!version.has_value() || !previous.has_value() || !merkle.has_value() || !time.has_value() ||
        !bits.has_value() || !nonce.has_value()) {
        return std::nullopt;
    }
    const primitives::BlockHeader header{*version, *previous, *merkle, *time, *bits, *nonce};
    return primitives::CheckBlockHeaderStructure(header) ==
                   primitives::BlockHeaderStructureError::kNone
               ? std::optional<primitives::BlockHeader>{header}
               : std::nullopt;
}

[[nodiscard]] bool WriteAddress(primitives::BinaryWriter& writer,
                                const NetworkAddress& address) noexcept
{
    return writer.WriteU64(address.services) && writer.WriteFixedBytes(address.address) &&
           WriteU16BigEndian(writer, address.port);
}

[[nodiscard]] std::optional<NetworkAddress> ReadAddress(primitives::BinaryReader& reader) noexcept
{
    const auto services = reader.ReadU64();
    const auto address = reader.ReadFixedBytes<16U>();
    const auto port = ReadU16BigEndian(reader);
    if (!services.has_value() || !address.has_value() || !port.has_value()) {
        return std::nullopt;
    }
    return NetworkAddress{*services, *address, *port};
}

[[nodiscard]] bool WriteInventory(primitives::BinaryWriter& writer,
                                  const InventoryVector& inventory) noexcept
{
    return writer.WriteU32(inventory.type) && writer.WriteHash256(inventory.hash);
}

[[nodiscard]] std::optional<InventoryVector>
ReadInventory(primitives::BinaryReader& reader) noexcept
{
    const auto type = reader.ReadU32();
    const auto hash = reader.ReadHash256();
    return type.has_value() && hash.has_value() ? std::optional<InventoryVector>{{*type, *hash}}
                                                : std::nullopt;
}

[[nodiscard]] bool
HasOnlyCanonicalCommandPadding(const std::array<std::uint8_t, kCommandSize>& bytes) noexcept
{
    bool found_nul = false;
    for (const auto byte : bytes) {
        if (byte == 0U) {
            found_nul = true;
        } else if (found_nul || byte < static_cast<std::uint8_t>('a') ||
                   byte > static_cast<std::uint8_t>('z')) {
            return false;
        }
    }
    return bytes.front() != 0U;
}

[[nodiscard]] std::uint32_t ReadLittleEndianU32(const std::vector<std::uint8_t>& bytes,
                                                const std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(bytes.at(offset)) |
           (static_cast<std::uint32_t>(bytes.at(offset + 1U)) << 8U) |
           (static_cast<std::uint32_t>(bytes.at(offset + 2U)) << 16U) |
           (static_cast<std::uint32_t>(bytes.at(offset + 3U)) << 24U);
}

[[nodiscard]] bool AppendFrameHeader(primitives::BinaryWriter& writer, const Command command,
                                     const std::uint32_t magic,
                                     const std::span<const std::uint8_t> payload) noexcept
{
    if (!writer.WriteU32(magic)) {
        return false;
    }
    const auto name = CommandToString(command);
    if (name.empty() || name.size() > kCommandSize) {
        return false;
    }
    for (std::size_t index = 0U; index < kCommandSize; ++index) {
        const auto byte = index < name.size() ? static_cast<std::uint8_t>(name.at(index))
                                              : static_cast<std::uint8_t>(0U);
        if (!writer.WriteU8(byte)) {
            return false;
        }
    }
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() ||
        !writer.WriteU32(static_cast<std::uint32_t>(payload.size()))) {
        return false;
    }
    const auto checksum = crypto::Hash256::DoubleSha256(payload);
    if (!checksum.has_value()) {
        return false;
    }
    for (std::size_t index = 0U; index < 4U; ++index) {
        if (!writer.WriteU8(checksum->bytes().at(index))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
SerializePayload(const FramedMessage& framed, const P2PParams& parameters) noexcept
{
    try {
        primitives::BinaryWriter writer;
        switch (framed.command) {
        case Command::kVersion: {
            const auto* version = std::get_if<VersionMessage>(&framed.message);
            if (version == nullptr || !WriteI32(writer, version->version) ||
                !writer.WriteU64(version->services) || !writer.WriteI64(version->timestamp) ||
                !writer.WriteU64(version->nonce) ||
                !writer.WriteString(version->user_agent, parameters.max_user_agent) ||
                !WriteI32(writer, version->start_height) ||
                !writer.WriteU8(version->relay ? 1U : 0U)) {
                return std::nullopt;
            }
            break;
        }
        case Command::kVerack:
        case Command::kGetAddr:
            if (!std::holds_alternative<std::monostate>(framed.message)) {
                return std::nullopt;
            }
            break;
        case Command::kPing:
        case Command::kPong: {
            const auto* nonce = std::get_if<NonceMessage>(&framed.message);
            if (nonce == nullptr || !writer.WriteU64(nonce->nonce)) {
                return std::nullopt;
            }
            break;
        }
        case Command::kAddr: {
            const auto* addresses = std::get_if<AddressMessage>(&framed.message);
            if (addresses == nullptr ||
                !writer.WriteArray<NetworkAddress>(addresses->addresses, parameters.max_addresses,
                                                   WriteAddress)) {
                return std::nullopt;
            }
            break;
        }
        case Command::kInv:
        case Command::kGetData: {
            const auto* inventory = std::get_if<InventoryMessage>(&framed.message);
            if (inventory == nullptr ||
                !writer.WriteArray<InventoryVector>(inventory->inventory, parameters.max_inventory,
                                                    WriteInventory)) {
                return std::nullopt;
            }
            break;
        }
        case Command::kTx: {
            const auto* transaction = std::get_if<primitives::Transaction>(&framed.message);
            if (transaction == nullptr ||
                !transaction->Serialize(writer, parameters.transaction_limits)) {
                return std::nullopt;
            }
            break;
        }
        case Command::kBlock: {
            const auto* block = std::get_if<primitives::Block>(&framed.message);
            if (block == nullptr || !block->Serialize(writer, parameters.block_limits)) {
                return std::nullopt;
            }
            break;
        }
        case Command::kGetHeaders: {
            const auto* request = std::get_if<GetHeadersMessage>(&framed.message);
            if (request == nullptr || !WriteI32(writer, request->version) ||
                !writer.WriteArray<crypto::Hash256>(
                    request->locator_hashes, parameters.max_locator_hashes,
                    [](primitives::BinaryWriter& array_writer, const crypto::Hash256& hash) {
                        return array_writer.WriteHash256(hash);
                    }) ||
                !writer.WriteHash256(request->stop_hash)) {
                return std::nullopt;
            }
            break;
        }
        case Command::kHeaders: {
            const auto* headers = std::get_if<HeadersMessage>(&framed.message);
            if (headers == nullptr || !writer.WriteArray<primitives::BlockHeader>(
                                          headers->headers, parameters.max_headers, WriteHeader)) {
                return std::nullopt;
            }
            break;
        }
        }
        return std::vector<std::uint8_t>{writer.bytes().begin(), writer.bytes().end()};
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

std::string_view CommandToString(const Command command) noexcept
{
    switch (command) {
    case Command::kVersion:
        return "version";
    case Command::kVerack:
        return "verack";
    case Command::kPing:
        return "ping";
    case Command::kPong:
        return "pong";
    case Command::kAddr:
        return "addr";
    case Command::kGetAddr:
        return "getaddr";
    case Command::kInv:
        return "inv";
    case Command::kGetData:
        return "getdata";
    case Command::kTx:
        return "tx";
    case Command::kBlock:
        return "block";
    case Command::kGetHeaders:
        return "getheaders";
    case Command::kHeaders:
        return "headers";
    }
    return {};
}

std::optional<Command> CommandFromString(const std::string_view command) noexcept
{
    for (const auto candidate :
         {Command::kVersion, Command::kVerack, Command::kPing, Command::kPong, Command::kAddr,
          Command::kGetAddr, Command::kInv, Command::kGetData, Command::kTx, Command::kBlock,
          Command::kGetHeaders, Command::kHeaders}) {
        if (command == CommandToString(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> SerializeMessage(const FramedMessage& message,
                                                          const P2PParams& parameters) noexcept
{
    const auto payload = SerializePayload(message, parameters);
    if (!payload.has_value() || payload->size() > parameters.max_payload) {
        return std::nullopt;
    }
    try {
        primitives::BinaryWriter writer;
        if (!AppendFrameHeader(writer, message.command, parameters.network_magic, *payload)) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> result{writer.bytes().begin(), writer.bytes().end()};
        result.insert(result.end(), payload->begin(), payload->end());
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<FramedMessage> DeserializeMessage(const Command command,
                                                const std::span<const std::uint8_t> payload,
                                                const P2PParams& parameters) noexcept
{
    if (payload.size() > parameters.max_payload) {
        return std::nullopt;
    }
    try {
        if (command == Command::kTx) {
            auto transaction =
                primitives::Transaction::Deserialize(payload, parameters.transaction_limits);
            return transaction.has_value()
                       ? std::optional<FramedMessage>{{command, std::move(*transaction)}}
                       : std::nullopt;
        }
        if (command == Command::kBlock) {
            auto block = primitives::Block::Deserialize(payload, parameters.block_limits);
            return block.has_value() ? std::optional<FramedMessage>{{command, std::move(*block)}}
                                     : std::nullopt;
        }
        primitives::BinaryReader reader{payload};
        switch (command) {
        case Command::kVersion: {
            const auto version = ReadI32(reader);
            const auto services = reader.ReadU64();
            const auto timestamp = reader.ReadI64();
            const auto nonce = reader.ReadU64();
            auto user_agent = reader.ReadString(parameters.max_user_agent);
            const auto height = ReadI32(reader);
            const auto relay = reader.ReadU8();
            if (!version.has_value() || !services.has_value() || !timestamp.has_value() ||
                !nonce.has_value() || !user_agent.has_value() || !height.has_value() ||
                !relay.has_value() || *version < parameters.minimum_protocol_version ||
                *relay > 1U || !reader.RequireEnd()) {
                return std::nullopt;
            }
            return FramedMessage{command,
                                 VersionMessage{*version, *services, *timestamp, *nonce,
                                                std::move(*user_agent), *height, *relay == 1U}};
        }
        case Command::kVerack:
        case Command::kGetAddr:
            return reader.RequireEnd() ? std::optional<FramedMessage>{{command, std::monostate{}}}
                                       : std::nullopt;
        case Command::kPing:
        case Command::kPong: {
            const auto nonce = reader.ReadU64();
            return nonce.has_value() && reader.RequireEnd()
                       ? std::optional<FramedMessage>{{command, NonceMessage{*nonce}}}
                       : std::nullopt;
        }
        case Command::kAddr: {
            auto addresses =
                reader.ReadArray<NetworkAddress>(parameters.max_addresses, 26U, ReadAddress);
            return addresses.has_value() && reader.RequireEnd()
                       ? std::optional<FramedMessage>{{command,
                                                       AddressMessage{std::move(*addresses)}}}
                       : std::nullopt;
        }
        case Command::kInv:
        case Command::kGetData: {
            auto inventory =
                reader.ReadArray<InventoryVector>(parameters.max_inventory, 36U, ReadInventory);
            return inventory.has_value() && reader.RequireEnd()
                       ? std::optional<FramedMessage>{{command,
                                                       InventoryMessage{std::move(*inventory)}}}
                       : std::nullopt;
        }
        case Command::kGetHeaders: {
            const auto version = ReadI32(reader);
            auto locator = reader.ReadArray<crypto::Hash256>(
                parameters.max_locator_hashes, crypto::Hash256::kSize,
                [](primitives::BinaryReader& array_reader) { return array_reader.ReadHash256(); });
            const auto stop = reader.ReadHash256();
            if (!version.has_value() || !locator.has_value() || !stop.has_value() ||
                !reader.RequireEnd()) {
                return std::nullopt;
            }
            return FramedMessage{command, GetHeadersMessage{*version, std::move(*locator), *stop}};
        }
        case Command::kHeaders: {
            auto headers =
                reader.ReadArray<primitives::BlockHeader>(parameters.max_headers, 80U, ReadHeader);
            return headers.has_value() && reader.RequireEnd()
                       ? std::optional<FramedMessage>{{command,
                                                       HeadersMessage{std::move(*headers)}}}
                       : std::nullopt;
        }
        case Command::kTx:
        case Command::kBlock:
            break;
        }
    } catch (...) {
        return std::nullopt;
    }
    return std::nullopt;
}

MessageParser::MessageParser(P2PParams parameters) noexcept : parameters_(parameters) {}

ParseResult MessageParser::PushBytes(const std::span<const std::uint8_t> bytes) noexcept
{
    if (parameters_.max_payload == 0U ||
        bytes.size() > kMessageHeaderSize + parameters_.max_payload ||
        buffered_.size() > kMessageHeaderSize + parameters_.max_payload - bytes.size()) {
        Reset();
        return {P2PError::kPayloadTooLarge, {}};
    }
    try {
        buffered_.insert(buffered_.end(), bytes.begin(), bytes.end());
        ParseResult result;
        while (buffered_.size() >= kMessageHeaderSize) {
            if (ReadLittleEndianU32(buffered_, 0U) != parameters_.network_magic) {
                Reset();
                return {P2PError::kUnexpectedMagic, {}};
            }
            std::array<std::uint8_t, kCommandSize> command_bytes{};
            std::copy_n(buffered_.begin() + 4, kCommandSize, command_bytes.begin());
            if (!HasOnlyCanonicalCommandPadding(command_bytes)) {
                Reset();
                return {P2PError::kMalformedCommand, {}};
            }
            const auto nul = std::find(command_bytes.begin(), command_bytes.end(), 0U);
            const std::string_view command_name{
                reinterpret_cast<const char*>(command_bytes.data()),
                static_cast<std::size_t>(nul - command_bytes.begin())};
            const auto command = CommandFromString(command_name);
            if (!command.has_value()) {
                Reset();
                return {P2PError::kUnknownCommand, {}};
            }
            const auto length = ReadLittleEndianU32(buffered_, 16U);
            if (length > parameters_.max_payload) {
                Reset();
                return {P2PError::kPayloadTooLarge, {}};
            }
            const auto frame_size = kMessageHeaderSize + static_cast<std::size_t>(length);
            if (buffered_.size() < frame_size) {
                break;
            }
            const std::span<const std::uint8_t> payload{buffered_.data() + kMessageHeaderSize,
                                                        length};
            const auto checksum = crypto::Hash256::DoubleSha256(payload);
            if (!checksum.has_value() || !std::equal(buffered_.begin() + 20, buffered_.begin() + 24,
                                                     checksum->bytes().begin())) {
                Reset();
                return {P2PError::kChecksumMismatch, {}};
            }
            auto message = DeserializeMessage(*command, payload, parameters_);
            if (!message.has_value()) {
                Reset();
                return {P2PError::kMalformedPayload, {}};
            }
            result.messages.push_back(std::move(*message));
            buffered_.erase(buffered_.begin(),
                            buffered_.begin() + static_cast<std::ptrdiff_t>(frame_size));
        }
        return result;
    } catch (...) {
        Reset();
        return {P2PError::kAllocationFailure, {}};
    }
}

void MessageParser::Reset() noexcept
{
    buffered_.clear();
}

Connection::Connection(const ConnectionDirection direction, ConnectionParams parameters,
                       const std::uint64_t now) noexcept
    : direction_(direction), parameters_(std::move(parameters)), parser_(parameters_.protocol),
      connected_at_(now), last_received_at_(now), window_started_at_(now)
{
}

P2PError Connection::Start() noexcept
{
    if (local_version_sent_) {
        return P2PError::kHandshakeViolation;
    }
    return CheckParameters() ? Queue({Command::kVersion, parameters_.local_version})
                             : P2PError::kInvalidParameters;
}

ConnectionResult Connection::Receive(const std::span<const std::uint8_t> bytes,
                                     const std::uint64_t now) noexcept
{
    if (!CheckParameters()) {
        return {P2PError::kInvalidParameters, {}};
    }
    if (now < last_received_at_ || now < window_started_at_ ||
        now - last_received_at_ > parameters_.idle_timeout) {
        return {P2PError::kTimeout, {}};
    }
    if (now - window_started_at_ >= parameters_.message_window) {
        window_started_at_ = now;
        messages_in_window_ = 0U;
    }
    const auto parsed = parser_.PushBytes(bytes);
    if (parsed.error != P2PError::kNone) {
        return {parsed.error, {}};
    }
    if (parsed.messages.size() > parameters_.max_messages_per_window - messages_in_window_) {
        return {P2PError::kMessageLimit, {}};
    }
    for (const auto& message : parsed.messages) {
        const auto handshake = ProcessHandshake(message);
        if (handshake != P2PError::kNone) {
            return {handshake, {}};
        }
        if (message.command == Command::kPing) {
            const auto* ping = std::get_if<NonceMessage>(&message.message);
            if (ping == nullptr || Queue({Command::kPong, *ping}) != P2PError::kNone) {
                return {P2PError::kQueueLimit, {}};
            }
        }
    }
    messages_in_window_ += static_cast<std::uint32_t>(parsed.messages.size());
    last_received_at_ = now;
    return {P2PError::kNone, parsed.messages};
}

P2PError Connection::Queue(const FramedMessage& message) noexcept
{
    auto serialized = SerializeMessage(message, parameters_.protocol);
    if (!serialized.has_value()) {
        return P2PError::kMalformedPayload;
    }
    if (outbound_.size() >= parameters_.max_queued_messages ||
        queued_bytes_ > parameters_.max_queued_bytes ||
        serialized->size() > parameters_.max_queued_bytes - queued_bytes_) {
        return P2PError::kQueueLimit;
    }
    try {
        outbound_.push_back(std::move(*serialized));
        queued_bytes_ += outbound_.back().size();
        if (message.command == Command::kVersion) {
            local_version_sent_ = true;
        }
        return P2PError::kNone;
    } catch (...) {
        return P2PError::kAllocationFailure;
    }
}

std::vector<std::vector<std::uint8_t>> Connection::TakeOutbound() noexcept
{
    queued_bytes_ = 0U;
    auto result = std::move(outbound_);
    outbound_.clear();
    return result;
}

P2PError Connection::Tick(const std::uint64_t now) noexcept
{
    if (now < connected_at_ || now < last_received_at_ ||
        (!established() && now - connected_at_ > parameters_.handshake_timeout) ||
        now - last_received_at_ > parameters_.idle_timeout) {
        return P2PError::kTimeout;
    }
    return P2PError::kNone;
}

bool Connection::established() const noexcept
{
    return local_version_sent_ && remote_version_received_ && remote_verack_received_;
}

ConnectionDirection Connection::direction() const noexcept
{
    return direction_;
}

bool Connection::CheckParameters() const noexcept
{
    return parameters_.protocol.network_magic != 0U &&
           parameters_.local_version.version >= parameters_.protocol.minimum_protocol_version &&
           parameters_.protocol.max_payload != 0U && parameters_.protocol.max_user_agent != 0U &&
           parameters_.protocol.max_addresses != 0U && parameters_.protocol.max_inventory != 0U &&
           parameters_.protocol.max_locator_hashes != 0U &&
           parameters_.protocol.max_headers != 0U && parameters_.handshake_timeout != 0U &&
           parameters_.idle_timeout != 0U && parameters_.message_window != 0U &&
           parameters_.max_messages_per_window != 0U && parameters_.max_queued_messages != 0U &&
           parameters_.max_queued_bytes != 0U;
}

P2PError Connection::ProcessHandshake(const FramedMessage& message) noexcept
{
    if (message.command == Command::kVersion) {
        if (remote_version_received_) {
            return P2PError::kHandshakeViolation;
        }
        remote_version_received_ = true;
        return Queue({Command::kVerack, std::monostate{}});
    }
    if (message.command == Command::kVerack) {
        if (!remote_version_received_ || remote_verack_received_) {
            return P2PError::kHandshakeViolation;
        }
        remote_verack_received_ = true;
        return P2PError::kNone;
    }
    return established() ? P2PError::kNone : P2PError::kHandshakeViolation;
}

PeerManager::PeerManager(PeerManagerParams parameters) noexcept : parameters_(std::move(parameters))
{
}

std::optional<std::uint64_t> PeerManager::AddInbound(const std::uint64_t now) noexcept
{
    return Add(ConnectionDirection::kInbound, now);
}

std::optional<std::uint64_t> PeerManager::AddOutbound(const std::uint64_t now) noexcept
{
    return Add(ConnectionDirection::kOutbound, now);
}

PeerReceiveResult PeerManager::Receive(const std::uint64_t peer_id,
                                       const std::span<const std::uint8_t> bytes,
                                       const std::uint64_t now) noexcept
{
    const auto peer = peers_.find(peer_id);
    if (peer == peers_.end()) {
        return {P2PError::kUnknownPeer, {}};
    }
    const auto received = peer->second.connection.Receive(bytes, now);
    if (received.error != P2PError::kNone) {
        Remove(peer_id);
        return {received.error, {}};
    }
    try {
        std::vector<PeerEvent> events;
        events.reserve(received.messages.size());
        for (auto message : received.messages) {
            events.push_back(PeerEvent{peer_id, std::move(message)});
        }
        return {P2PError::kNone, std::move(events)};
    } catch (...) {
        Remove(peer_id);
        return {P2PError::kAllocationFailure, {}};
    }
}

P2PError PeerManager::Queue(const std::uint64_t peer_id, const FramedMessage& message) noexcept
{
    const auto peer = peers_.find(peer_id);
    return peer == peers_.end() ? P2PError::kUnknownPeer : peer->second.connection.Queue(message);
}

std::vector<std::vector<std::uint8_t>>
PeerManager::TakeOutbound(const std::uint64_t peer_id) noexcept
{
    const auto peer = peers_.find(peer_id);
    return peer == peers_.end() ? std::vector<std::vector<std::uint8_t>>{}
                                : peer->second.connection.TakeOutbound();
}

void PeerManager::Tick(const std::uint64_t now) noexcept
{
    for (auto iterator = peers_.begin(); iterator != peers_.end();) {
        if (iterator->second.connection.Tick(now) != P2PError::kNone) {
            iterator = peers_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void PeerManager::Disconnect(const std::uint64_t peer_id) noexcept
{
    Remove(peer_id);
}

std::size_t PeerManager::peer_count() const noexcept
{
    return peers_.size();
}

std::optional<std::uint64_t> PeerManager::Add(const ConnectionDirection direction,
                                              const std::uint64_t now) noexcept
{
    std::uint32_t count = 0U;
    for (const auto& [peer_id, peer] : peers_) {
        static_cast<void>(peer_id);
        if (peer.direction == direction) {
            ++count;
        }
    }
    const auto limit = direction == ConnectionDirection::kInbound ? parameters_.max_inbound_peers
                                                                  : parameters_.max_outbound_peers;
    if (limit == 0U || count >= limit || next_peer_id_ == 0U) {
        return std::nullopt;
    }
    try {
        const auto id = next_peer_id_++;
        Connection connection{direction, parameters_.connection, now};
        if (connection.Start() != P2PError::kNone ||
            !peers_.emplace(id, Peer{id, direction, std::move(connection)}).second) {
            return std::nullopt;
        }
        return id;
    } catch (...) {
        return std::nullopt;
    }
}

void PeerManager::Remove(const std::uint64_t peer_id) noexcept
{
    peers_.erase(peer_id);
}

} // namespace nova::net
